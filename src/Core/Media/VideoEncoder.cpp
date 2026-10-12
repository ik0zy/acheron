#include "Core/Media/VideoEncoder.hpp"

#include "Core/Logging.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace Acheron {
namespace Core {
namespace Media {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

enum class EncoderFamily {
    Amf,
    Nvenc,
    Qsv,
    MediaFoundation,
    Vaapi,
    Vpx,
    X264,
    OpenH264,
};

struct VideoEncoderCandidate
{
    // ffmpeg's name for the encoder, except where one ffmpeg encoder is two candidates.
    const char *label;
    const char *ffmpegName;
    VideoCodec codec;
    EncoderFamily family;
    bool hardware;
    // Which pictures it takes as Direct3D 11 frames: NV12 ones, and the screen's own BGRA.
    bool takesCardFrames;
    bool takesRgbCardFrames;
    // Whether a new bit rate written into its context is picked up while it runs.
    bool followsBitrate;
};

namespace {

// In the order they are tried. Media Foundation is the graphics card's encoder or Microsoft's
// software one, by an option. The software one runs pictures behind (sixteen, where it was
// measured) whatever it is told through ffmpeg, so the trial turns it down. libopenh264 and
// x264 are left out on Windows on purpose.
#if defined(Q_OS_WIN)
constexpr std::array CANDIDATES = {
    VideoEncoderCandidate{ "h264_amf", "h264_amf", VideoCodec::H264, EncoderFamily::Amf, true, true, true, false },
    VideoEncoderCandidate{ "h264_nvenc", "h264_nvenc", VideoCodec::H264, EncoderFamily::Nvenc, true, true, true, true },
    VideoEncoderCandidate{ "h264_qsv", "h264_qsv", VideoCodec::H264, EncoderFamily::Qsv, true, false, false, true },
    VideoEncoderCandidate{ "h264_mf hardware", "h264_mf", VideoCodec::H264, EncoderFamily::MediaFoundation, true, true, false, false },
    VideoEncoderCandidate{ "h264_mf", "h264_mf", VideoCodec::H264, EncoderFamily::MediaFoundation, false, false, false, false },
    VideoEncoderCandidate{ "libvpx", "libvpx", VideoCodec::VP8, EncoderFamily::Vpx, false, false, false, false },
};
#else
constexpr std::array CANDIDATES = {
    VideoEncoderCandidate{ "h264_vaapi", "h264_vaapi", VideoCodec::H264, EncoderFamily::Vaapi, true, false, false, false },
    VideoEncoderCandidate{ "h264_nvenc", "h264_nvenc", VideoCodec::H264, EncoderFamily::Nvenc, true, false, false, true },
    VideoEncoderCandidate{ "libx264", "libx264", VideoCodec::H264, EncoderFamily::X264, false, false, false, false },
    VideoEncoderCandidate{ "libopenh264", "libopenh264", VideoCodec::H264, EncoderFamily::OpenH264, false, false, false, false },
    VideoEncoderCandidate{ "libvpx", "libvpx", VideoCodec::VP8, EncoderFamily::Vpx, false, false, false, false },
};
#endif

constexpr int RTP_CLOCK = 90000;

// ffmpeg's numbers for the profiles, which it has renamed between versions.
constexpr int H264_PROFILE_MAIN = 77;
constexpr int H264_PROFILE_HIGH = 100;
constexpr std::array H264_PROFILES = { H264_PROFILE_HIGH, H264_PROFILE_MAIN };

// The official client stops its encoders at 56 of VP8's 63 quantizer steps. This is the same
// share of H264's 51.
constexpr int VP8_MIN_QUANTIZER = 2;
constexpr int VP8_MAX_QUANTIZER = 56;
constexpr int H264_MAX_QP = 45;

constexpr int NAL_IDR = 5;
constexpr int NAL_SPS = 7;
constexpr int NAL_PPS = 8;
constexpr int NAL_ACCESS_UNIT_DELIMITER = 9;
constexpr std::array<uint8_t, 4> START_CODE = { 0, 0, 0, 1 };

constexpr int TRIAL_WIDTH = 640;
constexpr int TRIAL_HEIGHT = 360;
constexpr int TRIAL_PICTURES = 3;
constexpr int TRIAL_BITRATE = 1'000'000;

struct Nal
{
    size_t begin;
    size_t end;
};

// The NAL units of an Annex B stream, without their start codes.
std::vector<Nal> splitNals(std::span<const uint8_t> data)
{
    std::vector<Nal> nals;
    for (size_t i = 0; i + 3 <= data.size();) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
            i++;
            continue;
        }
        if (!nals.empty())
            nals.back().end = data[i - 1] == 0 ? i - 1 : i;
        nals.push_back({ i + 3, data.size() });
        i += 3;
    }
    return nals;
}

bool startsWithStartCode(std::span<const uint8_t> data, const std::vector<Nal> &nals)
{
    return !nals.empty() && (nals.front().begin == 3 || (nals.front().begin == 4 && data[0] == 0));
}

int nalType(std::span<const uint8_t> data, const Nal &nal)
{
    return nal.begin < nal.end ? data[nal.begin] & 0x1F : -1;
}

void appendNal(std::vector<uint8_t> &to, std::span<const uint8_t> nal)
{
    to.insert(to.end(), START_CODE.begin(), START_CODE.end());
    to.insert(to.end(), nal.begin(), nal.end());
}

// The thread counts libwebrtc gives libvpx, short of the eight it uses on the largest machines.
int vpxThreads(int width, int height)
{
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int pixels = width * height;
    if (pixels > 1280 * 960 && cores >= 6)
        return 3;
    if (pixels > 640 * 480 && cores >= 3)
        return 2;
    return 1;
}

bool encodesTrialPictures(const VideoEncoderCandidate &candidate)
{
    if (!avcodec_find_encoder_by_name(candidate.ffmpegName))
        return false;

    VideoEncoder::Settings settings;
    settings.codec = candidate.codec;
    settings.bitrate = TRIAL_BITRATE;
    settings.encoderName = QString::fromLatin1(candidate.label);
    VideoEncoder encoder(settings);

    AVFrame *picture = av_frame_alloc();
    picture->format = encoder.softwareFormat();
    picture->width = TRIAL_WIDTH;
    picture->height = TRIAL_HEIGHT;
    bool worked = av_frame_get_buffer(picture, 0) >= 0;

    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; worked && index < TRIAL_PICTURES; index++) {
        av_frame_make_writable(picture);
        for (int y = 0; y < TRIAL_HEIGHT; y++)
            memset(picture->data[0] + y * picture->linesize[0], (y + index * 16) & 0xFF, TRIAL_WIDTH);
        for (int plane = 1; plane < AV_NUM_DATA_POINTERS && picture->data[plane]; plane++)
            memset(picture->data[plane], 128, static_cast<size_t>(picture->linesize[plane]) * (TRIAL_HEIGHT / 2));

        const int64_t timestamp = int64_t(index) * RTP_CLOCK / settings.framerate;
        const size_t before = frames.size();
        worked = encoder.encode(picture, timestamp, frames) == VideoEncoder::Outcome::Encoded;
        // A frame that comes out a picture late would stay late for as long as the stream runs.
        for (size_t i = before; i < frames.size(); i++)
            worked = worked && frames[i].rtpTimestamp == static_cast<uint32_t>(timestamp);
    }
    av_frame_free(&picture);

    // Rate control may drop a picture, even one of these.
    return worked && !frames.empty() && frames.front().keyframe && frames.size() + 1 >= TRIAL_PICTURES;
}

// Whether the encoder opens and encodes on this machine: tried once, and remembered.
bool candidateWorks(const VideoEncoderCandidate &candidate)
{
    enum class Verdict {
        Untried,
        Works,
        Broken,
    };
    struct Verdicts
    {
        std::mutex mutex;
        std::array<Verdict, CANDIDATES.size()> of = {};
    };
    static Verdicts verdicts;

    std::lock_guard lock(verdicts.mutex);
    Verdict &verdict = verdicts.of[static_cast<size_t>(&candidate - CANDIDATES.data())];
    if (verdict == Verdict::Untried) {
        verdict = encodesTrialPictures(candidate) ? Verdict::Works : Verdict::Broken;
        qCInfo(LogVideo) << "Encoder" << candidate.label << (verdict == Verdict::Works ? "works" : "does not work on this machine");
    }
    return verdict == Verdict::Works;
}

} // namespace

QList<Discord::Voice::VideoCodec> encodableVideoCodecs()
{
    static const QList<VideoCodec> codecs = [] {
        QList<VideoCodec> found;
        for (VideoCodec codec : { VideoCodec::H264, VideoCodec::VP8 }) {
            if (std::any_of(CANDIDATES.begin(), CANDIDATES.end(), [codec](const VideoEncoderCandidate &each) { return each.codec == codec && candidateWorks(each); }))
                found.append(codec);
        }
        return found;
    }();
    return codecs;
}

QList<QString> videoEncoderNames(Discord::Voice::VideoCodec codec)
{
    QList<QString> names;
    for (const VideoEncoderCandidate &each : CANDIDATES) {
        if (each.codec == codec && avcodec_find_encoder_by_name(each.ffmpegName))
            names.append(QString::fromLatin1(each.label));
    }
    return names;
}

bool videoEncoderWorks(const QString &name)
{
    return std::any_of(CANDIDATES.begin(), CANDIDATES.end(), [&name](const VideoEncoderCandidate &each) { return name == QLatin1String(each.label) && candidateWorks(each); });
}

VideoEncoder::VideoEncoder(const Settings &settings)
    : settings(settings), uploaded(av_frame_alloc()), packet(av_packet_alloc())
{
    for (const VideoEncoderCandidate &each : CANDIDATES) {
        if (each.codec != settings.codec)
            continue;
        if (settings.encoderName.isEmpty() ? candidateWorks(each) : settings.encoderName == QLatin1String(each.label)) {
            candidate = &each;
            break;
        }
    }
}

VideoEncoder::~VideoEncoder()
{
    close();
    av_frame_free(&uploaded);
    av_packet_free(&packet);
}

bool VideoEncoder::takesCardFrames() const
{
    return candidate && candidate->takesCardFrames && !cardFramesRefused;
}

bool VideoEncoder::takesRgbCardFrames() const
{
    return takesCardFrames() && candidate->takesRgbCardFrames;
}

int VideoEncoder::softwareFormat() const
{
    return candidate && candidate->family == EncoderFamily::Vpx ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
}

void VideoEncoder::requestKeyframe()
{
    keyframeWanted = true;
}

void VideoEncoder::setBitrate(int bitrate)
{
    if (bitrate == settings.bitrate)
        return;
    settings.bitrate = bitrate;
    if (!context)
        return;

    if (!candidate->followsBitrate) {
        reopenWanted = true;
        return;
    }
    context->bit_rate = bitrate;
    context->rc_max_rate = bitrate;
    context->rc_buffer_size = bufferBits();
}

VideoEncoder::Outcome VideoEncoder::encode(AVFrame *frame, int64_t timestamp, std::vector<EncodedVideoFrame> &out)
{
    const bool onCard = frame->hw_frames_ctx != nullptr;
    if (!onCard && candidate && frame->format != softwareFormat())
        return Outcome::Failed;

    const bool rgbOnCard = onCard && reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data)->sw_format != AV_PIX_FMT_NV12;
    while (candidate) {
        if (onCard && !(rgbOnCard ? takesRgbCardFrames() : takesCardFrames()))
            return Outcome::CardFramesRefused;
        if (encodeWithOpenEncoder(frame, timestamp, out))
            return Outcome::Encoded;

        // One that had been working gets a fresh start on the same picture. One that fails
        // again straight away is at fault, unless the picture came on the card: that it turns
        // those down says nothing about what it does with pictures from memory.
        const bool hadBeenWorking = framesSinceOpen >= TRIAL_FRAMES;
        close();
        if (hadBeenWorking)
            continue;
        if (onCard) {
            cardFramesRefused = true;
            return Outcome::CardFramesRefused;
        }
        moveToNextEncoder();
    }
    return Outcome::Failed;
}

void VideoEncoder::moveToNextEncoder()
{
    const VideoEncoderCandidate *failed = candidate;
    candidate = nullptr;
    if (settings.encoderName.isEmpty()) {
        for (const VideoEncoderCandidate *next = failed + 1; next != CANDIDATES.data() + CANDIDATES.size() && !candidate; next++) {
            if (next->codec == failed->codec && candidateWorks(*next))
                candidate = next;
        }
    }
    cardFramesRefused = false;
    qCWarning(LogVideo) << "Encoder" << failed->label << "failed;" << (candidate ? "using" : "no other encoder for") << (candidate ? QString::fromLatin1(candidate->label) : Discord::Voice::videoCodecName(failed->codec));
}

bool VideoEncoder::encodeWithOpenEncoder(AVFrame *frame, int64_t timestamp, std::vector<EncodedVideoFrame> &out)
{
    const bool onCard = frame->hw_frames_ctx != nullptr;
    // AMF asserts that a picture on the card belongs to the frames context it was opened with.
    const bool sameSource = context && (onCard ? context->hw_frames_ctx && !uploadFrames && context->hw_frames_ctx->data == frame->hw_frames_ctx->data : uploadFrames || context->pix_fmt == frame->format);
    const bool fits = sameSource && !reopenWanted && context->width == frame->width && context->height == frame->height;
    if (!fits && !open(frame))
        return false;

    QElapsedTimer timer;
    timer.start();

    const bool intervalPassed = settings.keyframeIntervalMs > 0 && timestamp - lastKeyframeAt >= int64_t(settings.keyframeIntervalMs) * RTP_CLOCK / 1000;
    const bool keyframe = framesSinceOpen == 0 || keyframeWanted || intervalPassed;

    AVFrame *input = uploadFrames ? uploadedCopy(frame) : frame;
    if (!input)
        return false;
    // AMF makes whatever kind of picture a frame says it is, so an ordinary one says nothing.
    input->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    input->pts = timestamp;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
    input->duration = RTP_CLOCK / std::max(1, settings.framerate);
#endif

    const int sent = avcodec_send_frame(context, input);
    av_frame_unref(uploaded);
    if (sent < 0)
        return false;
    keyframeWanted = false;
    awaited++;

    const size_t before = out.size();
    if (!collect(out))
        return false;
    framesSinceOpen++;

    if (out.size() > before) {
        if (out[before].keyframe)
            lastKeyframeAt = timestamp;
        else if (keyframe) {
            qCWarning(LogVideo) << "Encoder" << candidate->label << "made no key frame when asked for one; starting it over";
            reopenWanted = true;
        }
    }

    const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    counters.averageEncodeMs += (elapsedMs - counters.averageEncodeMs) / std::min<double>(static_cast<double>(framesSinceOpen), 30.0);
    counters.encoded += out.size() - before;
    return true;
}

bool VideoEncoder::collect(std::vector<EncodedVideoFrame> &out)
{
    QElapsedTimer waited;
    waited.start();

    for (;;) {
        const int received = avcodec_receive_packet(context, packet);
        if (received == AVERROR(EAGAIN)) {
            // AMF takes a picture from memory and answers some milliseconds later, saying
            // "again" until then; each asking waits a millisecond inside AMF, so this does not
            // spin. The others have answered by now or have dropped the picture.
            if (candidate->family != EncoderFamily::Amf || awaited == 0)
                return true;
            if (waited.elapsed() >= OUTPUT_WAIT_MS) {
                awaited = 0;
                return true;
            }
            std::this_thread::yield();
            continue;
        }
        if (received < 0)
            return false;

        EncodedVideoFrame encoded;
        encoded.data.assign(packet->data, packet->data + packet->size);
        encoded.rtpTimestamp = static_cast<uint32_t>(packet->pts);
        av_packet_unref(packet);
        awaited = std::max(0, awaited - 1);

        if (encoded.data.empty())
            continue;
        if (candidate->codec == VideoCodec::H264) {
            if (!finishH264(encoded))
                return false;
        } else {
            // VP8 says so itself, in the lowest bit of its first byte.
            encoded.keyframe = !(encoded.data[0] & 1);
        }
        out.push_back(std::move(encoded));
    }
}

// Whether a frame is a key frame is read from the frame, and one that does not open with its
// parameter sets is given them: which encoders repeat them, and when, differs by encoder and
// by how it was asked.
bool VideoEncoder::finishH264(EncodedVideoFrame &frame)
{
    const std::span<const uint8_t> data(frame.data);
    const std::vector<Nal> nals = splitNals(data);
    if (!startsWithStartCode(data, nals))
        return false;

    bool hasIdr = false;
    for (const Nal &nal : nals) {
        const int type = nalType(data, nal);
        hasIdr = hasIdr || type == NAL_IDR;
        if (type == NAL_SPS)
            sps.assign(data.begin() + nal.begin, data.begin() + nal.end);
        if (type == NAL_PPS)
            pps.assign(data.begin() + nal.begin, data.begin() + nal.end);
    }
    frame.keyframe = hasIdr;
    if (!hasIdr || (nals.size() > 2 && nalType(data, nals[0]) == NAL_SPS && nalType(data, nals[1]) == NAL_PPS))
        return true;
    if (sps.empty() || pps.empty())
        return false;

    std::vector<uint8_t> ordered;
    ordered.reserve(frame.data.size() + sps.size() + pps.size() + 2 * START_CODE.size());
    appendNal(ordered, sps);
    appendNal(ordered, pps);
    for (const Nal &nal : nals) {
        const int type = nalType(data, nal);
        if (type != NAL_SPS && type != NAL_PPS && type != NAL_ACCESS_UNIT_DELIMITER)
            appendNal(ordered, data.subspan(nal.begin, nal.end - nal.begin));
    }
    frame.data = std::move(ordered);
    return true;
}

void VideoEncoder::rememberParameterSets(const uint8_t *data, size_t size)
{
    sps.clear();
    pps.clear();
    if (!data)
        return;

    const std::span<const uint8_t> extradata(data, size);
    const std::vector<Nal> nals = splitNals(extradata);
    if (!startsWithStartCode(extradata, nals))
        return;
    for (const Nal &nal : nals) {
        if (nalType(extradata, nal) == NAL_SPS)
            sps.assign(extradata.begin() + nal.begin, extradata.begin() + nal.end);
        if (nalType(extradata, nal) == NAL_PPS)
            pps.assign(extradata.begin() + nal.begin, extradata.begin() + nal.end);
    }
}

// The official client gives its encoders a buffer of about one frame. libvpx is the exception:
// it drops pictures whenever its buffer runs low, and gets the second libwebrtc gives it.
int VideoEncoder::bufferBits() const
{
    if (candidate->family == EncoderFamily::Vpx)
        return settings.bitrate;
    return settings.bitrate / std::max(1, settings.framerate);
}

AVCodecContext *VideoEncoder::configured(const AVFrame *frame, int h264Profile) const
{
    const AVCodec *encoder = avcodec_find_encoder_by_name(candidate->ffmpegName);
    AVCodecContext *made = encoder ? avcodec_alloc_context3(encoder) : nullptr;
    if (!made)
        return nullptr;

    made->width = frame->width;
    made->height = frame->height;
    made->time_base = { 1, RTP_CLOCK };
    made->framerate = { std::max(1, settings.framerate), 1 };
    made->colorspace = frame->colorspace;
    made->color_range = frame->color_range;
    made->color_primaries = frame->color_primaries;
    made->color_trc = frame->color_trc;
    if (uploadFrames) {
        made->pix_fmt = AV_PIX_FMT_VAAPI;
        made->hw_frames_ctx = av_buffer_ref(uploadFrames);
    } else {
        made->pix_fmt = static_cast<AVPixelFormat>(frame->format);
        if (frame->hw_frames_ctx)
            made->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
    }

    // What the official client's encoders are set to: a constant bit rate, no B-frames, and no
    // key frames but the ones asked for.
    made->bit_rate = settings.bitrate;
    made->rc_max_rate = settings.bitrate;
    made->rc_buffer_size = bufferBits();
    made->max_b_frames = 0;
    made->gop_size = INT_MAX;
    made->flags |= AV_CODEC_FLAG_LOW_DELAY;

    void *options = made->priv_data;
    if (candidate->codec == VideoCodec::H264) {
        made->profile = h264Profile;
        av_opt_set(options, "profile", h264Profile == H264_PROFILE_HIGH ? "high" : "main", 0);
        made->qmax = H264_MAX_QP;
    }

    // An option an encoder does not have, in this ffmpeg or at all, is left unset.
    switch (candidate->family) {
    case EncoderFamily::Amf:
        av_opt_set(options, "usage", "lowlatency", 0);
        av_opt_set(options, "rc", "cbr", 0);
        av_opt_set_int(options, "bf", 0, 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        av_opt_set_int(options, "forced_idr", 1, 0);
        // Its count of frames between key frames, where none is zero.
        made->gop_size = 0;
        break;
    case EncoderFamily::Nvenc:
        av_opt_set(options, "preset", "p4", 0);
        av_opt_set(options, "tune", "ull", 0);
        av_opt_set(options, "rc", "cbr", 0);
        av_opt_set_int(options, "zerolatency", 1, 0);
        av_opt_set_int(options, "delay", 0, 0);
        av_opt_set_int(options, "forced-idr", 1, 0);
        break;
    case EncoderFamily::Qsv:
        av_opt_set(options, "scenario", "displayremoting", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        av_opt_set_int(options, "forced_idr", 1, 0);
        // It keeps the count in sixteen bits.
        made->gop_size = UINT16_MAX;
        break;
    case EncoderFamily::MediaFoundation:
        av_opt_set_int(options, "hw_encoding", candidate->hardware ? 1 : 0, 0);
        av_opt_set(options, "rate_control", "cbr", 0);
        break;
    case EncoderFamily::Vaapi:
        av_opt_set(options, "rc_mode", "CBR", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        break;
    case EncoderFamily::Vpx:
        // The settings of libwebrtc's VP8 encoder for a screen.
        made->rc_min_rate = settings.bitrate;
        made->rc_initial_buffer_occupancy = settings.bitrate / 2;
        made->qmin = VP8_MIN_QUANTIZER;
        made->qmax = VP8_MAX_QUANTIZER;
        made->thread_count = vpxThreads(frame->width, frame->height);
        av_opt_set(options, "deadline", "realtime", 0);
        av_opt_set_int(options, "cpu-used", -6, 0);
        // Left alone it holds back 25 frames.
        av_opt_set_int(options, "lag-in-frames", 0, 0);
        av_opt_set_int(options, "auto-alt-ref", 0, 0);
        av_opt_set_int(options, "drop-threshold", 30, 0);
        av_opt_set_int(options, "undershoot-pct", 100, 0);
        av_opt_set_int(options, "overshoot-pct", 15, 0);
        av_opt_set_int(options, "static-thresh", 100, 0);
        av_opt_set_int(options, "max-intra-rate", std::max(300, 30 * settings.framerate), 0);
        break;
    case EncoderFamily::X264:
        av_opt_set(options, "preset", "veryfast", 0);
        av_opt_set(options, "tune", "zerolatency", 0);
        av_opt_set_int(options, "forced-idr", 1, 0);
        break;
    case EncoderFamily::OpenH264:
        av_opt_set_int(options, "allow_skip_frames", 1, 0);
        break;
    }
    return made;
}

bool VideoEncoder::open(const AVFrame *frame)
{
    close();
    reopenWanted = false;
    if (candidate->family == EncoderFamily::Vaapi && !prepareUpload(frame))
        return false;

    const size_t profiles = candidate->codec == VideoCodec::H264 ? H264_PROFILES.size() : 1;
    for (size_t i = 0; i < profiles && !context; i++) {
        AVCodecContext *made = configured(frame, H264_PROFILES[i]);
        if (made && avcodec_open2(made, nullptr, nullptr) < 0)
            avcodec_free_context(&made);
        context = made;
    }
    if (!context)
        return false;

    rememberParameterSets(context->extradata, static_cast<size_t>(context->extradata_size));
    awaited = 0;

    counters.encoderName = QString::fromLatin1(candidate->label);
    if (frame->hw_frames_ctx)
        counters.encoderName += QStringLiteral(" (%1)").arg(QString::fromUtf8(av_hwdevice_get_type_name(reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data)->device_ctx->type)));
    counters.hardware = candidate->hardware;
    counters.width = frame->width;
    counters.height = frame->height;
    counters.opened++;
    qCInfo(LogVideo) << "Encoding" << Discord::Voice::videoCodecName(candidate->codec) << "with" << counters.encoderName << "at" << frame->width << "x" << frame->height << settings.bitrate / 1000 << "kbps";
    return true;
}

// An encoder that only takes pictures from its own kind of device gets them copied there.
bool VideoEncoder::prepareUpload(const AVFrame *frame)
{
    AVBufferRef *device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
        return false;
    uploadFrames = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);
    if (!uploadFrames)
        return false;

    auto *frames = reinterpret_cast<AVHWFramesContext *>(uploadFrames->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = static_cast<AVPixelFormat>(frame->format);
    frames->width = frame->width;
    frames->height = frame->height;
    if (av_hwframe_ctx_init(uploadFrames) < 0) {
        av_buffer_unref(&uploadFrames);
        return false;
    }
    return true;
}

AVFrame *VideoEncoder::uploadedCopy(const AVFrame *frame)
{
    av_frame_unref(uploaded);
    if (av_hwframe_get_buffer(uploadFrames, uploaded, 0) < 0 || av_hwframe_transfer_data(uploaded, frame, 0) < 0)
        return nullptr;
    av_frame_copy_props(uploaded, frame);
    return uploaded;
}

void VideoEncoder::close()
{
    avcodec_free_context(&context);
    av_buffer_unref(&uploadFrames);
    framesSinceOpen = 0;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
