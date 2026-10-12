// Real encodes. Synthetic pictures go through every encoder this machine has and back through
// the decoder, and the pipeline test shares the actual screen. What the screen showed is only
// ever counted and measured here: never printed, never written anywhere.

#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/ScreenCapture.hpp"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTest>
#include <QThread>

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#ifdef Q_OS_WIN
#include <windows.h>

#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

using namespace Acheron;
using namespace Acheron::Discord::Voice;
using Core::Media::VideoEncoder;

namespace {

constexpr int WIDTH = 640;
constexpr int HEIGHT = 360;
constexpr int FRAMERATE = 30;
constexpr int TICKS_PER_FRAME = 90000 / FRAMERATE;
constexpr int BITRATE = 1'500'000;

// The colour of the patch in the middle of a synthetic RGB picture.
constexpr int PATCH_RED = 40;
constexpr int PATCH_GREEN = 200;
constexpr int PATCH_BLUE = 60;

struct EncoderCase
{
    VideoCodec codec = VideoCodec::Unknown;
    // Empty for whichever encoder the probe settled on.
    QString encoderName;
    // For the tests that take one: the format of the pictures, or the tallest picture to send.
    int detail = 0;
};

struct Yuv
{
    double y = 0;
    double u = 0;
    double v = 0;
};

// What the patch comes to in video-range YUV under either set of coefficients.
Yuv patchInYuv(bool bt709)
{
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double luma = kr * PATCH_RED + (1 - kr - kb) * PATCH_GREEN + kb * PATCH_BLUE;
    return { 16 + 219 * luma / 255, 128 + 224 * (PATCH_BLUE - luma) / (2 * (1 - kb)) / 255, 128 + 224 * (PATCH_RED - luma) / (2 * (1 - kr)) / 255 };
}

double distance(const Yuv &a, const Yuv &b)
{
    return std::sqrt((a.y - b.y) * (a.y - b.y) + (a.u - b.u) * (a.u - b.u) + (a.v - b.v) * (a.v - b.v));
}

// Counts decoded pictures, and keeps the colour in the middle of the last one.
class CountingSink : public Core::Media::VideoFrameSink
{
public:
    void present(const AVFrame *frame) override
    {
        width = frame->width;
        height = frame->height;
        colorspace = frame->colorspace;
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            const int x = frame->width / 2;
            const int y = frame->height / 2;
            middleY = frame->data[0][y * frame->linesize[0] + x];
            middleU = frame->data[1][(y / 2) * frame->linesize[1] + x / 2];
            middleV = frame->data[2][(y / 2) * frame->linesize[2] + x / 2];
        }
        frames++;
    }

    std::atomic<int> frames{ 0 };
    std::atomic<int> width{ 0 };
    std::atomic<int> height{ 0 };
    std::atomic<int> colorspace{ AVCOL_SPC_UNSPECIFIED };
    std::atomic<int> middleY{ 0 };
    std::atomic<int> middleU{ 0 };
    std::atomic<int> middleV{ 0 };
};

// A gradient that drifts, a patch of fine texture that slides across it, and a band of noise
// that is new in every picture: enough that an encoder has to spend what it is given.
void paintYuv(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            uint8_t value = static_cast<uint8_t>((x + y) / 4 + index * 3);
            if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
            if (x < width / 4 && y < height / 6) {
                noise = noise * 1664525u + 1013904223u;
                value = static_cast<uint8_t>(96 + (noise >> 26));
            }
            row[x] = value;
        }
    }

    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width / 2; x++) {
            const auto u = static_cast<uint8_t>(128 + y / 4 + index * 2);
            const auto v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            if (interleaved) {
                picture->data[1][y * picture->linesize[1] + 2 * x] = u;
                picture->data[1][y * picture->linesize[1] + 2 * x + 1] = v;
            } else {
                picture->data[1][y * picture->linesize[1] + x] = u;
                picture->data[2][y * picture->linesize[2] + x] = v;
            }
        }
    }
}

// Drifting gradients around a patch of one colour, which is what the colour checks look at.
void paintBgra(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            const bool inPatch = std::abs(x - width / 2) < width / 8 && std::abs(y - height / 2) < height / 8;
            row[4 * x] = inPatch ? PATCH_BLUE : static_cast<uint8_t>(x / 2 + index * 3);
            row[4 * x + 1] = inPatch ? PATCH_GREEN : static_cast<uint8_t>(y / 2 + index * 2);
            row[4 * x + 2] = inPatch ? PATCH_RED : static_cast<uint8_t>((x + y) / 4);
            row[4 * x + 3] = 255;
        }
    }
}

void paint(AVFrame *picture, int index)
{
    av_frame_make_writable(picture);
    if (picture->format == AV_PIX_FMT_BGRA)
        paintBgra(picture, index);
    else
        paintYuv(picture, index);
}

AVFrame *allocatePicture(int format, int width, int height)
{
    AVFrame *picture = av_frame_alloc();
    picture->format = format;
    picture->width = width;
    picture->height = height;
    av_frame_get_buffer(picture, 0);
    return picture;
}

VideoEncoder::Settings settingsFor(const EncoderCase &encoderCase, int bitrate, int keyframeIntervalMs = 0)
{
    VideoEncoder::Settings settings;
    settings.codec = encoderCase.codec;
    settings.framerate = FRAMERATE;
    settings.bitrate = bitrate;
    settings.keyframeIntervalMs = keyframeIntervalMs;
    settings.encoderName = encoderCase.encoderName;
    return settings;
}

// One encoder fed synthetic pictures from memory, with what each call gave back.
class EncodeRun
{
public:
    EncodeRun(const EncoderCase &encoderCase, int width, int height, int bitrate, int keyframeIntervalMs = 0)
        : encoder(settingsFor(encoderCase, bitrate, keyframeIntervalMs)), picture(allocatePicture(encoder.softwareFormat(), width, height))
    {
    }

    ~EncodeRun() { av_frame_free(&picture); }

    void paintNext() { paint(picture, sent); }

    // False when the encoder could not be used at all.
    bool encodeNext(bool painting = true)
    {
        if (painting)
            paintNext();
        const size_t before = frames.size();
        const uint32_t timestamp = static_cast<uint32_t>(sent) * TICKS_PER_FRAME;

        QElapsedTimer timer;
        timer.start();
        const VideoEncoder::Outcome outcome = encoder.encode(picture, timestamp, frames);
        callNs.push_back(timer.nsecsElapsed());
        if (outcome != VideoEncoder::Outcome::Encoded)
            return false;

        // A frame belongs to the picture that went in with the same call, or it was held back.
        producedItsOwnFrame.push_back(frames.size() == before + 1 && frames.back().rtpTimestamp == timestamp);
        sent++;
        return true;
    }

    [[nodiscard]] double averageCallMs(size_t from) const
    {
        qint64 total = 0;
        for (size_t i = from; i < callNs.size(); i++)
            total += callNs[i];
        return callNs.size() > from ? static_cast<double>(total) / 1e6 / static_cast<double>(callNs.size() - from) : 0;
    }

    [[nodiscard]] double bitsPerSecond(size_t from, size_t to, int pictures) const
    {
        size_t bytes = 0;
        for (size_t i = from; i < to; i++)
            bytes += frames[i].data.size();
        return static_cast<double>(bytes) * 8.0 * FRAMERATE / pictures;
    }

    VideoEncoder encoder;
    std::vector<EncodedVideoFrame> frames;
    std::vector<bool> producedItsOwnFrame;
    std::vector<qint64> callNs;
    int sent = 0;

private:
    AVFrame *picture;
};

#ifdef Q_OS_WIN
// Synthetic pictures put on the graphics card the way captured ones are found there.
class CardPictures
{
public:
    CardPictures(AVPixelFormat format, int width, int height, int colorspace)
        : colorspace(colorspace), inMemory(allocatePicture(format, width, height)), onCard(av_frame_alloc())
    {
        AVBufferRef *device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
            return;
        frames = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);

        auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
        context->format = AV_PIX_FMT_D3D11;
        context->sw_format = format;
        context->width = width;
        context->height = height;
        static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
        if (av_hwframe_ctx_init(frames) < 0)
            av_buffer_unref(&frames);
    }

    ~CardPictures()
    {
        av_frame_free(&inMemory);
        av_frame_free(&onCard);
        av_buffer_unref(&frames);
    }

    [[nodiscard]] bool usable() const { return frames != nullptr; }

    AVFrame *next(int index)
    {
        paint(inMemory, index);
        av_frame_unref(onCard);
        if (av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0)
            return nullptr;
        onCard->colorspace = static_cast<AVColorSpace>(colorspace);
        onCard->color_range = AVCOL_RANGE_MPEG;
        return onCard;
    }

private:
    int colorspace;
    AVBufferRef *frames = nullptr;
    AVFrame *inMemory;
    AVFrame *onCard;
};

// Whether the card makes NV12 textures of the kind a frames context of this shape asks for.
bool cardMakesNv12Textures(int poolSize, unsigned bindFlags)
{
    AVBufferRef *device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
        return false;
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);

    auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
    context->format = AV_PIX_FMT_D3D11;
    context->sw_format = AV_PIX_FMT_NV12;
    context->width = 1280;
    context->height = 720;
    context->initial_pool_size = poolSize;
    static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = bindFlags;
    bool made = av_hwframe_ctx_init(frames) >= 0;
    if (made) {
        AVFrame *frame = av_frame_alloc();
        made = av_hwframe_get_buffer(frames, frame, 0) >= 0;
        av_frame_free(&frame);
    }
    av_buffer_unref(&frames);
    return made;
}

// The processor time this process has used on all its threads, in milliseconds. Counted in
// processor cycles: the times Windows keeps are sampled at its clock tick, which misses most
// of the work of threads that wake up for a few milliseconds at a time.
double processCpuMs()
{
    static const double cyclesPerMs = [] {
        constexpr qint64 CALIBRATION_NS = 20'000'000;
        ULONG64 before = 0;
        ULONG64 after = 0;
        QueryThreadCycleTime(GetCurrentThread(), &before);
        QElapsedTimer spinning;
        spinning.start();
        while (spinning.nsecsElapsed() < CALIBRATION_NS) {}
        QueryThreadCycleTime(GetCurrentThread(), &after);
        return static_cast<double>(after - before) / (static_cast<double>(CALIBRATION_NS) / 1e6);
    }();

    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return static_cast<double>(cycles) / cyclesPerMs;
}
#else
double processCpuMs()
{
    return 0;
}
#endif

bool startsWithStartCode(const std::vector<uint8_t> &data)
{
    if (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;
    return data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

std::vector<int> nalTypes(const std::vector<uint8_t> &data)
{
    std::vector<int> types;
    for (size_t i = 0; i + 3 < data.size(); i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            types.push_back(data[i + 3] & 0x1F);
    }
    return types;
}

// The profile_idc of the first SPS in the frame, or -1.
int h264Profile(const std::vector<uint8_t> &data)
{
    for (size_t i = 0; i + 4 < data.size(); i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1F) == 7)
            return data[i + 4];
    }
    return -1;
}

// An H264 key frame is Annex B and opens with SPS and PPS; a VP8 frame says itself what it is.
bool isWellFormed(VideoCodec codec, const EncodedVideoFrame &frame)
{
    if (frame.data.empty())
        return false;
    if (codec == VideoCodec::VP8)
        return !(frame.data[0] & 1) == frame.keyframe;

    const std::vector<int> types = nalTypes(frame.data);
    const bool hasIdr = std::find(types.begin(), types.end(), 5) != types.end();
    if (!startsWithStartCode(frame.data) || hasIdr != frame.keyframe)
        return false;
    return !frame.keyframe || (types.size() >= 3 && types[0] == 7 && types[1] == 8);
}

struct Decoded
{
    int pictures = 0;
    int width = 0;
    int height = 0;
    quint64 errors = 0;
    quint64 dropped = 0;
    int colorspace = AVCOL_SPC_UNSPECIFIED;
    Yuv middle;
};

// Decodes the frames in order, in software.
template <typename Frames>
Decoded decodeAll(VideoCodec codec, const Frames &frames)
{
    auto sink = std::make_shared<CountingSink>();
    Core::Media::VideoDecoder decoder(false);
    decoder.setSink(sink);

    int pushed = 0;
    QElapsedTimer timer;
    timer.start();
    for (const auto &frame : frames) {
        decoder.push(codec, EncodedVideoFrame(*frame));
        pushed++;
        // The decoder throws its queue away when it falls far behind.
        while (pushed - sink->frames > 20 && timer.elapsed() < 10000)
            QThread::msleep(1);
    }
    while (sink->frames < pushed && timer.elapsed() < 10000)
        QThread::msleep(2);
    QThread::msleep(50);

    Decoded decoded;
    decoded.pictures = sink->frames;
    decoded.width = sink->width;
    decoded.height = sink->height;
    decoded.errors = decoder.stats().errors;
    decoded.dropped = decoder.stats().dropped;
    decoded.colorspace = sink->colorspace;
    decoded.middle = { static_cast<double>(sink->middleY), static_cast<double>(sink->middleU), static_cast<double>(sink->middleV) };
    return decoded;
}

Decoded decodeAll(VideoCodec codec, const std::vector<EncodedVideoFrame> &frames)
{
    std::vector<const EncodedVideoFrame *> pointers;
    for (const EncodedVideoFrame &frame : frames)
        pointers.push_back(&frame);
    return decodeAll(codec, pointers);
}

} // namespace

Q_DECLARE_METATYPE(EncoderCase)

// A named encoder the probe turned down is not one the rest of the tests can say anything about.
#define SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase)                                                     \
    if (!(encoderCase).encoderName.isEmpty() && !Core::Media::videoEncoderWorks((encoderCase).encoderName)) \
    QSKIP("this encoder does not open on this machine, or holds frames back")

class TestVideoEncode : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void aSourceIsFittedInsideTheSendSize();
    void theProbeGivesTheSameAnswerEveryTime();
    void encodableCodecsAreOfferedForSending();
    void theProbeTurnsDownWhatCannotKeepUp_data() { encoderCases(false); }
    void theProbeTurnsDownWhatCannotKeepUp();

    void everyPictureComesOutAtOnceAndDecodes_data() { encoderCases(true); }
    void everyPictureComesOutAtOnceAndDecodes();
    void aChangedBitrateIsFollowed_data() { encoderCases(true); }
    void aChangedBitrateIsFollowed();
    void keyFramesComeAtTheIntervalAskedFor_data() { encoderCases(true); }
    void keyFramesComeAtTheIntervalAskedFor();
    void aChangedPictureSizeStartsANewStream_data() { encoderCases(true); }
    void aChangedPictureSizeStartsANewStream();
    void encodeTimes_data() { encoderCases(true); }
    void encodeTimes();

    void picturesOnTheGraphicsCardAreEncodedThere_data();
    void picturesOnTheGraphicsCardAreEncodedThere();
    void whichTexturesTheCardMakes();

    void screensAreListedWithThumbnails();
    void thePipelineSharesTheScreen_data();
    void thePipelineSharesTheScreen();
    void aScreenThatIsNotThereFails();

private:
    static void encoderCases(bool withTheProbesChoice);
};

void TestVideoEncode::encoderCases(bool withTheProbesChoice)
{
    QTest::addColumn<EncoderCase>("encoderCase");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : { VideoCodec::H264, VideoCodec::VP8 }) {
        if (withTheProbesChoice && encodable.contains(codec))
            QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" as the probe chose"))) << EncoderCase{ codec, {} };
        for (const QString &name : Core::Media::videoEncoderNames(codec))
            QTest::newRow(qPrintable(name)) << EncoderCase{ codec, name };
    }
}

void TestVideoEncode::initTestCase()
{
    QStringList codecs;
    for (VideoCodec codec : Core::Media::encodableVideoCodecs())
        codecs.append(videoCodecName(codec));
    qInfo() << "encodable codecs:" << codecs;
    for (VideoCodec codec : { VideoCodec::H264, VideoCodec::VP8 }) {
        for (const QString &name : Core::Media::videoEncoderNames(codec))
            qInfo().noquote() << videoCodecName(codec) << "encoder" << name << (Core::Media::videoEncoderWorks(name) ? "works" : "is turned down by the probe");
    }
}

void TestVideoEncode::aSourceIsFittedInsideTheSendSize()
{
    using Core::Video::encodeSizeFor;
    QCOMPARE(encodeSizeFor(QSize(1920, 1080), 720), QSize(1280, 720));
    QCOMPARE(encodeSizeFor(QSize(2560, 1440), 1080), QSize(1920, 1080));
    QCOMPARE(encodeSizeFor(QSize(3840, 2160), 0), QSize(3840, 2160));
    // Never enlarged.
    QCOMPARE(encodeSizeFor(QSize(1280, 720), 1080), QSize(1280, 720));
    QCOMPARE(encodeSizeFor(QSize(1366, 768), 1080), QSize(1366, 768));
    // Wider than 16:9 is held by the width, taller by the height.
    QCOMPARE(encodeSizeFor(QSize(3440, 1440), 720), QSize(1280, 534));
    QCOMPARE(encodeSizeFor(QSize(1440, 2560), 720), QSize(404, 720));
    QCOMPARE(encodeSizeFor(QSize(1920, 1200), 720), QSize(1152, 720));
    // Both sides even, also when nothing is scaled.
    QCOMPARE(encodeSizeFor(QSize(1365, 767), 0), QSize(1364, 766));
    QCOMPARE(encodeSizeFor(QSize(1920, 1080), 480), QSize(852, 480));
}

void TestVideoEncode::theProbeGivesTheSameAnswerEveryTime()
{
    const QList<VideoCodec> first = Core::Media::encodableVideoCodecs();
    for (int i = 0; i < 3; i++)
        QVERIFY(Core::Media::encodableVideoCodecs() == first);
    QVERIFY(Core::Video::BroadcastPipeline::encodableCodecs() == first);

    // Only what the RTP sender packetizes, H264 before VP8.
    QList<VideoCodec> expected;
    for (VideoCodec codec : { VideoCodec::H264, VideoCodec::VP8 }) {
        if (first.contains(codec))
            expected.append(codec);
    }
    QVERIFY(first == expected);

    // A codec is encodable exactly when one of its encoders works, and then it can be encoded.
    for (VideoCodec codec : { VideoCodec::H264, VideoCodec::VP8 }) {
        const QList<QString> names = Core::Media::videoEncoderNames(codec);
        const bool oneWorks = std::any_of(names.begin(), names.end(), [](const QString &name) { return Core::Media::videoEncoderWorks(name); });
        QCOMPARE(first.contains(codec), oneWorks);

        EncodeRun run({ codec, {} }, WIDTH, HEIGHT, BITRATE);
        QCOMPARE(run.encoder.hasEncoder(), oneWorks);
        QCOMPARE(run.encodeNext(), oneWorks);
    }
    EncodeRun unsupported({ VideoCodec::AV1, {} }, WIDTH, HEIGHT, BITRATE);
    QVERIFY(!unsupported.encodeNext());
}

void TestVideoEncode::encodableCodecsAreOfferedForSending()
{
    const QList<VideoCodec> decodable = Core::Media::decodableVideoCodecs();
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    const QList<Codec> offered = Core::Media::offeredVideoCodecs(encodable);
    QCOMPARE(offered.size(), decodable.size());
    for (int i = 0; i < offered.size(); i++) {
        QCOMPARE(offered[i].name.get(), videoCodecName(decodable[i]));
        QCOMPARE(offered[i].decode.get(), true);
        QCOMPARE(offered[i].encode.get(), encodable.contains(decodable[i]));
        QCOMPARE(offered[i].payloadType.get(), 101 + 2 * i);
    }
    for (const Codec &codec : Core::Media::offeredVideoCodecs())
        QCOMPARE(codec.encode.get(), false);
}

// The probe's verdict on an encoder is that it opens and gives every frame back with the
// picture it was made from. Microsoft's software H264 encoder opens and then runs sixteen
// pictures behind, which is why it is not used.
void TestVideoEncode::theProbeTurnsDownWhatCannotKeepUp()
{
    QFETCH(EncoderCase, encoderCase);
    constexpr int PICTURES = 30;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    bool opens = true;
    for (int index = 0; index < PICTURES && opens; index++)
        opens = run.encodeNext();
    const int withTheirPicture = static_cast<int>(std::count(run.producedItsOwnFrame.begin(), run.producedItsOwnFrame.end(), true));
    qInfo().noquote() << encoderCase.encoderName << (opens ? "opens;" : "does not open;") << run.frames.size() << "frames from" << run.sent << "pictures," << withTheirPicture << "of them with the picture's own call";

    const bool keepsUp = opens && withTheirPicture == PICTURES && run.frames[0].keyframe;
    QCOMPARE(Core::Media::videoEncoderWorks(encoderCase.encoderName), keepsUp);
}

void TestVideoEncode::everyPictureComesOutAtOnceAndDecodes()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 75;
    const std::vector<int> forced = { 20, 21, 47 };

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    for (int index = 0; index < PICTURES; index++) {
        if (std::find(forced.begin(), forced.end(), index) != forced.end())
            run.encoder.requestKeyframe();
        QVERIFY2(run.encodeNext(), qPrintable(QStringLiteral("picture %1 could not be encoded").arg(index)));
    }
    qInfo() << "encoder:" << run.encoder.stats().encoderName << "hardware:" << run.encoder.stats().hardware;
    if (encoderCase.codec == VideoCodec::H264) {
        // Main or High, as the official client sends: not Baseline.
        const int profile = h264Profile(run.frames[0].data);
        qInfo() << "H264 profile_idc:" << profile;
        QVERIFY(profile == 77 || profile == 100);
    }

    // One picture in, its frame out of the same call: nothing is held back.
    QCOMPARE(run.frames.size(), size_t(PICTURES));
    for (int index = 0; index < PICTURES; index++)
        QVERIFY2(run.producedItsOwnFrame[static_cast<size_t>(index)], qPrintable(QStringLiteral("picture %1 did not come out with its own call").arg(index)));
    QCOMPARE(run.encoder.stats().opened, quint64(1));
    QCOMPARE(run.encoder.stats().encoded, quint64(PICTURES));

    // Key frames where they were asked for and nowhere else: none come by themselves.
    for (int index = 0; index < PICTURES; index++) {
        const bool expected = index == 0 || std::find(forced.begin(), forced.end(), index) != forced.end();
        QVERIFY2(run.frames[static_cast<size_t>(index)].keyframe == expected, qPrintable(QStringLiteral("frame %1: key frame %2, expected %3").arg(index).arg(run.frames[static_cast<size_t>(index)].keyframe).arg(expected)));
    }
    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));

    const Decoded whole = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(whole.errors, quint64(0));
    QCOMPARE(whole.dropped, quint64(0));
    QCOMPARE(whole.pictures, PICTURES);
    QCOMPARE(whole.width, WIDTH);
    QCOMPARE(whole.height, HEIGHT);

    // A key frame needs nothing that came before it, and what follows needs only the key frame.
    for (size_t index = 0; index < run.frames.size(); index++) {
        if (!run.frames[index].keyframe)
            continue;
        const Decoded alone = decodeAll(encoderCase.codec, std::vector<EncodedVideoFrame>{ run.frames[index] });
        QCOMPARE(alone.errors, quint64(0));
        QCOMPARE(alone.pictures, 1);
        QCOMPARE(alone.width, WIDTH);

        const std::vector<EncodedVideoFrame> fromHere(run.frames.begin() + static_cast<std::ptrdiff_t>(index), run.frames.end());
        const Decoded rest = decodeAll(encoderCase.codec, fromHere);
        QCOMPARE(rest.errors, quint64(0));
        QCOMPARE(rest.pictures, static_cast<int>(fromHere.size()));
    }
}

void TestVideoEncode::aChangedBitrateIsFollowed()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES_PER_RATE = 90;
    constexpr int SETTLING = 30;
    constexpr int LOWER_BITRATE = 500'000;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    size_t settledBefore = 0;
    for (int index = 0; index < PICTURES_PER_RATE; index++) {
        if (index == SETTLING)
            settledBefore = run.frames.size();
        QVERIFY(run.encodeNext());
    }
    const size_t atChange = run.frames.size();

    run.encoder.setBitrate(LOWER_BITRATE);
    size_t settledAfter = 0;
    for (int index = 0; index < PICTURES_PER_RATE; index++) {
        if (index == SETTLING)
            settledAfter = run.frames.size();
        QVERIFY(run.encodeNext());
    }

    // Measured against the pictures that went in, so that a dropped one counts as no bits.
    const double before = run.bitsPerSecond(settledBefore, atChange, PICTURES_PER_RATE - SETTLING);
    const double after = run.bitsPerSecond(settledAfter, run.frames.size(), PICTURES_PER_RATE - SETTLING);
    qInfo() << "encoder:" << run.encoder.stats().encoderName << "asked" << BITRATE << "made" << qRound(before) << "; asked" << LOWER_BITRATE << "made" << qRound(after) << "; opened" << run.encoder.stats().opened << "time(s);" << run.frames.size() << "frames from" << run.sent << "pictures";

    // The pictures are hard enough to use up either rate, so the stream sits near each.
    QVERIFY2(before > BITRATE * 0.7 && before < BITRATE * 1.3, qPrintable(QString::number(before)));
    QVERIFY2(after > LOWER_BITRATE * 0.7 && after < LOWER_BITRATE * 1.3, qPrintable(QString::number(after)));

    // An encoder that had to start over for the new rate starts with a key frame, and either
    // way everything that came out decodes as one stream.
    if (run.encoder.stats().opened > 1)
        QVERIFY(run.frames[atChange].keyframe);
    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));
    const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(run.frames.size()));
}

void TestVideoEncode::keyFramesComeAtTheIntervalAskedFor()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int INTERVAL_MS = 500;
    constexpr int PICTURES = 50;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE, INTERVAL_MS);
    for (int index = 0; index < PICTURES; index++)
        QVERIFY(run.encodeNext());

    QCOMPARE(run.frames.size(), size_t(PICTURES));
    const int framesPerInterval = INTERVAL_MS * FRAMERATE / 1000;
    for (int index = 0; index < PICTURES; index++)
        QCOMPARE(run.frames[static_cast<size_t>(index)].keyframe, index % framesPerInterval == 0);
}

void TestVideoEncode::aChangedPictureSizeStartsANewStream()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 10;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    QVERIFY(run.encodeNext());

    // The same encoder is handed pictures of another size, as after a change of screen mode.
    AVFrame *larger = allocatePicture(run.encoder.softwareFormat(), 960, 540);
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        paint(larger, index);
        QVERIFY(run.encoder.encode(larger, (index + 1) * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
    }
    av_frame_free(&larger);

    QCOMPARE(frames.size(), size_t(PICTURES));
    QVERIFY(frames[0].keyframe);
    QVERIFY(!frames[1].keyframe);
    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, PICTURES);
    QCOMPARE(decoded.width, 960);
    QCOMPARE(decoded.height, 540);
}

void TestVideoEncode::encodeTimes()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 60;
    constexpr int OPENING = 1;

    for (const QSize &size : { QSize(1280, 720), QSize(1920, 1080) }) {
        EncodeRun run(encoderCase, size.width(), size.height(), size.height() >= 1080 ? 6'000'000 : 3'500'000);
        QVERIFY(run.encodeNext());
        // Painting a picture is this test's own work and is kept out of the processor time.
        double processorMs = 0;
        for (int index = OPENING; index < PICTURES; index++) {
            run.paintNext();
            const double before = processCpuMs();
            QVERIFY(run.encodeNext(false));
            processorMs += processCpuMs() - before;
        }
        qInfo().noquote() << QStringLiteral("%1 %2x%3 from memory: %4 ms a picture, %5 ms of it processor time, opening took %6 ms").arg(run.encoder.stats().encoderName).arg(size.width()).arg(size.height()).arg(run.averageCallMs(OPENING), 0, 'f', 2).arg(processorMs / (PICTURES - OPENING), 0, 'f', 2).arg(static_cast<double>(run.callNs[0]) / 1e6, 0, 'f', 0);
        // A picture every 33 ms leaves no room for an encoder slower than that.
        QVERIFY(run.averageCallMs(OPENING) < 1000.0 / FRAMERATE);
    }
}

void TestVideoEncode::picturesOnTheGraphicsCardAreEncodedThere_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : { VideoCodec::H264, VideoCodec::VP8 }) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            QTest::newRow(qPrintable(name + QStringLiteral(", NV12"))) << EncoderCase{ codec, name, AV_PIX_FMT_NV12 };
            QTest::newRow(qPrintable(name + QStringLiteral(", BGRA"))) << EncoderCase{ codec, name, AV_PIX_FMT_BGRA };
        }
    }
}

void TestVideoEncode::picturesOnTheGraphicsCardAreEncodedThere()
{
#ifdef Q_OS_WIN
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 40;
    constexpr int FORCED = 20;
    const bool rgb = encoderCase.detail == AV_PIX_FMT_BGRA;

    {
        // RGB pictures are converted by the encoder, to the BT.709 they are marked with.
        CardPictures pictures(static_cast<AVPixelFormat>(encoderCase.detail), WIDTH, HEIGHT, AVCOL_SPC_BT709);
        if (!pictures.usable())
            QSKIP("no Direct3D 11 device takes these pictures");

        VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
        AVFrame *fromMemory = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
        const auto freeFromMemory = qScopeGuard([&fromMemory] { av_frame_free(&fromMemory); });
        paint(fromMemory, 0);
        std::vector<EncodedVideoFrame> frames;

        if (!(rgb ? encoder.takesRgbCardFrames() : encoder.takesCardFrames())) {
            // Turned down, and nothing held against the encoder: it goes on from memory.
            AVFrame *picture = pictures.next(0);
            QVERIFY(picture);
            QVERIFY(encoder.encode(picture, 0, frames) == VideoEncoder::Outcome::CardFramesRefused);
            QVERIFY(frames.empty());
            QVERIFY(encoder.encode(fromMemory, TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
            QCOMPARE(frames.size(), size_t(1));
            QVERIFY(frames[0].keyframe);
            return;
        }

        for (int index = 0; index < PICTURES; index++) {
            if (index == FORCED)
                encoder.requestKeyframe();
            AVFrame *picture = pictures.next(index);
            QVERIFY(picture);
            QVERIFY2(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded, qPrintable(QStringLiteral("picture %1 was not taken from the card").arg(index)));
            QCOMPARE(frames.size(), size_t(index + 1));
            QCOMPARE(frames.back().rtpTimestamp, uint32_t(index * TICKS_PER_FRAME));
        }
        qInfo() << "encoder:" << encoder.stats().encoderName << (rgb ? "BGRA" : "NV12") << "pictures," << encoder.stats().averageEncodeMs << "ms a picture";
        QVERIFY(encoder.stats().encoderName.contains(QStringLiteral("d3d11va")));
        QCOMPARE(encoder.stats().opened, quint64(1));
        for (int index = 0; index < PICTURES; index++) {
            QCOMPARE(frames[static_cast<size_t>(index)].keyframe, index == 0 || index == FORCED);
            QVERIFY(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
        }

        const Decoded decoded = decodeAll(encoderCase.codec, frames);
        QCOMPARE(decoded.errors, quint64(0));
        QCOMPARE(decoded.pictures, PICTURES);
        QCOMPARE(decoded.width, WIDTH);
        QCOMPARE(decoded.height, HEIGHT);
        if (rgb) {
            // And the stream says so, or a small picture would be shown as BT.601.
            qInfo() << "the patch decodes to" << decoded.middle.y << decoded.middle.u << decoded.middle.v << ", off BT.709 by" << distance(decoded.middle, patchInYuv(true)) << "and off BT.601 by" << distance(decoded.middle, patchInYuv(false));
            QVERIFY(distance(decoded.middle, patchInYuv(true)) < 6);
            QVERIFY(distance(decoded.middle, patchInYuv(false)) > 12);
            QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
        }

        // The same encoder is then given pictures from memory, as when the capture has to
        // fall back, and starts a new stream for them.
        QVERIFY(encoder.encode(fromMemory, PICTURES * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
        QCOMPARE(frames.size(), size_t(PICTURES + 1));
        QVERIFY(frames.back().keyframe);
        QVERIFY(!encoder.stats().encoderName.contains(QStringLiteral("d3d11va")));
    }
#else
    QSKIP("pictures stay on the graphics card only on Windows");
#endif
}

// Not a check of this code: a record of why ffmpeg's scale_d3d11 does or does not work here.
// It asks for a pool of ten NV12 textures in one array that are render targets and encoder
// inputs at once.
void TestVideoEncode::whichTexturesTheCardMakes()
{
#ifdef Q_OS_WIN
    struct Shape
    {
        const char *name;
        int poolSize;
        unsigned bindFlags;
    };
    const std::array shapes = {
        Shape{ "an array of ten, render target and encoder input (what scale_d3d11 asks for)", 10, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER },
        Shape{ "an array of ten, render target", 10, D3D11_BIND_RENDER_TARGET },
        Shape{ "single, render target and encoder input", 0, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER },
        Shape{ "single, render target", 0, D3D11_BIND_RENDER_TARGET },
    };
    for (const Shape &shape : shapes)
        qInfo().noquote() << "NV12 textures," << shape.name << ":" << (cardMakesNv12Textures(shape.poolSize, shape.bindFlags) ? "made" : "refused");
#else
    QSKIP("Direct3D 11 is on Windows");
#endif
}

void TestVideoEncode::screensAreListedWithThumbnails()
{
    if (!Core::Video::screenCaptureAvailable()) {
        QVERIFY(Core::Video::captureSources().isEmpty());
        QSKIP("this build or this machine cannot capture a screen");
    }
    const QList<Core::Video::CaptureSource> sources = Core::Video::captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");

    const QSize bound(320, 180);
    for (int i = 0; i < sources.size(); i++) {
        QCOMPARE(sources[i].id, QStringLiteral("screen:%1").arg(i));
        QVERIFY(!sources[i].name.isEmpty());
        QVERIFY(sources[i].geometry.width() >= 320 && sources[i].geometry.height() >= 200);

        QElapsedTimer timer;
        timer.start();
        const QImage thumbnail = Core::Video::captureThumbnail(sources[i], bound);
        qInfo() << sources[i].id << sources[i].name << sources[i].geometry << "thumbnail" << thumbnail.size() << "in" << timer.elapsed() << "ms";
        QVERIFY(!thumbnail.isNull());
        QCOMPARE(thumbnail.size(), sources[i].geometry.size().scaled(bound, Qt::KeepAspectRatio));
        QCOMPARE(thumbnail.format(), QImage::Format_RGB32);
        QCOMPARE(*reinterpret_cast<const quint32 *>(thumbnail.constScanLine(0)) >> 24, 0xFFu);
    }
    QVERIFY(Core::Video::captureThumbnail({}, bound).isNull());
}

void TestVideoEncode::thePipelineSharesTheScreen_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : Core::Media::encodableVideoCodecs()) {
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at 720p"))) << EncoderCase{ codec, {}, 720 };
        // Nothing to scale, which is when the pictures can go to the encoder as captured.
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at the screen's own size"))) << EncoderCase{ codec, {}, 0 };
    }
}

void TestVideoEncode::thePipelineSharesTheScreen()
{
    QFETCH(EncoderCase, encoderCase);
    using namespace Core::Video;
    if (!screenCaptureAvailable())
        QSKIP("this build or this machine cannot capture a screen");
    const QList<CaptureSource> sources = captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");

    BroadcastPipeline pipeline;
    QObject here;
    std::vector<EncodedFramePointer> frames;
    QList<QSize> starts;
    QList<QImage> previews;
    QList<BroadcastStats> stats;
    QList<bool> stalls;
    QStringList failures;
    connect(&pipeline, &BroadcastPipeline::started, &here, [&](const QSize &size, int framerate) {
        QCOMPARE(framerate, FRAMERATE);
        starts.append(size);
    });
    connect(&pipeline, &BroadcastPipeline::frameEncoded, &here, [&](const EncodedFramePointer &frame) { frames.push_back(frame); });
    connect(&pipeline, &BroadcastPipeline::previewReady, &here, [&](const QImage &preview) { previews.append(preview); });
    connect(&pipeline, &BroadcastPipeline::statsUpdated, &here, [&](const BroadcastStats &update) { stats.append(update); });
    connect(&pipeline, &BroadcastPipeline::captureStalled, &here, [&](bool stalled) { stalls.append(stalled); });
    connect(&pipeline, &BroadcastPipeline::failed, &here, [&](const QString &reason) { failures.append(reason); });

    BroadcastSettings settings;
    settings.sourceId = sources.first().id;
    settings.maxHeight = encoderCase.detail;
    settings.framerate = FRAMERATE;
    settings.codec = encoderCase.codec;
    settings.bitrate = 2'500'000;
    settings.maxBitrate = 4'000'000;

    QElapsedTimer sinceStart;
    sinceStart.start();
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(frames.size() >= 20 || !failures.isEmpty(), "fewer than 20 frames in 3 s", 3000);
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char(';'))));
    qInfo() << "20 frames after" << sinceStart.elapsed() << "ms";

    const QSize expectedSize = encodeSizeFor(sources.first().geometry.size(), encoderCase.detail);
    QCOMPARE(starts.size(), 1);
    QCOMPARE(starts.first(), expectedSize);
    QVERIFY(frames[0]->keyframe);
    QVERIFY(!frames[1]->keyframe);

    // Time stamps are when each picture was captured, on a 90 kHz clock.
    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(frames[i]->rtpTimestamp - frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(frames[19]->rtpTimestamp - frames[0]->rtpTimestamp) / 19.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8 && ticksPerFrame < TICKS_PER_FRAME * 1.25, qPrintable(QString::number(ticksPerFrame)));

    // A key frame comes with the next picture or two after it is asked for: the pictures
    // already on their way here when the request went out are the allowance.
    const size_t atRequest = frames.size();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= atRequest + 6, 1000);
    const auto forced = std::find_if(frames.begin() + static_cast<std::ptrdiff_t>(atRequest), frames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; });
    QVERIFY(forced != frames.end());
    QVERIFY(forced - frames.begin() < static_cast<std::ptrdiff_t>(atRequest) + 4);

    // What sharing costs this process while the test itself sits still.
    const double cpuBefore = processCpuMs();
    const size_t framesBefore = frames.size();
    QTest::qWait(1000);
    const double cpuPerFrame = (processCpuMs() - cpuBefore) / static_cast<double>(std::max<size_t>(1, frames.size() - framesBefore));

    // Nobody watching: nothing is encoded, and the previews carry on.
    QTRY_VERIFY_WITH_TIMEOUT(!previews.isEmpty(), 1000);
    pipeline.setEncodingSuspended(true);
    QTest::qWait(300);
    const size_t whileSuspended = frames.size();
    const int previewsBefore = previews.size();
    QTest::qWait(1200);
    QCOMPARE(frames.size(), whileSuspended);
    QVERIFY(previews.size() >= previewsBefore + 2);

    pipeline.setEncodingSuspended(false);
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= whileSuspended + 10, 1500);
    QVERIFY(frames[whileSuspended]->keyframe);
    QVERIFY(!frames[whileSuspended + 1]->keyframe);

    // A rate the encoder is moved to while it runs.
    pipeline.setBitrate(1'000'000);
    const size_t atNewRate = frames.size();
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= atNewRate + 15, 1500);

    for (const QImage &preview : previews) {
        QVERIFY(!preview.isNull());
        QVERIFY(preview.width() <= 512 && preview.height() <= 288);
        QCOMPARE(preview.size(), expectedSize.scaled(QSize(512, 288), Qt::KeepAspectRatio));
    }

    QTRY_VERIFY_WITH_TIMEOUT(stats.size() >= 2, 2500);
    const BroadcastStats lastStats = stats.last();
    qInfo().noquote() << QStringLiteral("pipeline: %1, hardware %2, %3x%4, captured %5 fps, %6 ms in the encoder a picture, %7 ms of processor time a picture in all, %8 frames, %9 dropped").arg(lastStats.encoderName).arg(lastStats.hardware).arg(lastStats.encodedSize.width()).arg(lastStats.encodedSize.height()).arg(lastStats.capturedFps, 0, 'f', 1).arg(lastStats.averageEncodeMs, 0, 'f', 2).arg(cpuPerFrame, 0, 'f', 2).arg(lastStats.framesEncoded).arg(lastStats.framesDropped);
    QVERIFY(!lastStats.encoderName.isEmpty());
    QCOMPARE(lastStats.encodedSize, expectedSize);
    QVERIFY(lastStats.capturedFps > FRAMERATE * 0.8 && lastStats.capturedFps < FRAMERATE * 1.2);

    pipeline.stop();
    QTest::qWait(300);
    const size_t afterStop = frames.size();
    QTest::qWait(300);
    QCOMPARE(frames.size(), afterStop);
    QVERIFY(failures.isEmpty());
    QVERIFY(stalls.isEmpty());

    // Everything sent decodes as one stream, the gap and the new rate included.
    for (const EncodedFramePointer &frame : frames)
        QVERIFY(isWellFormed(encoderCase.codec, *frame));
    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames.size()));
    QCOMPARE(QSize(decoded.width, decoded.height), expectedSize);

    // The same pipeline shares again, and time does not run backwards across the restart.
    const uint32_t lastTimestamp = frames.back()->rtpTimestamp;
    frames.clear();
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(frames.size() >= 5 || !failures.isEmpty(), "no frames after a restart", 3000);
    QVERIFY(failures.isEmpty());
    QCOMPARE(starts.size(), 2);
    QVERIFY(frames[0]->keyframe);
    QVERIFY(static_cast<int32_t>(frames[0]->rtpTimestamp - lastTimestamp) > 0);
}

void TestVideoEncode::aScreenThatIsNotThereFails()
{
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");

    BroadcastPipeline pipeline;
    QObject here;
    QStringList failures;
    int framesSeen = 0;
    int starts = 0;
    connect(&pipeline, &BroadcastPipeline::failed, &here, [&](const QString &reason) { failures.append(reason); });
    connect(&pipeline, &BroadcastPipeline::frameEncoded, &here, [&](const EncodedFramePointer &) { framesSeen++; });
    connect(&pipeline, &BroadcastPipeline::started, &here, [&](const QSize &, int) { starts++; });

    BroadcastSettings settings;
    settings.sourceId = QStringLiteral("screen:99");
    settings.codec = encodable.first();
    settings.bitrate = 1'000'000;
    pipeline.start(settings);

    // Not at once: a screen may be away for a moment and come back.
    QTest::qWait(1000);
    if (screenCaptureAvailable())
        QVERIFY(failures.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, ScreenCapture::GIVE_UP_AFTER_MS + 2000);
    qInfo() << "failed with:" << failures.first();
    QVERIFY(!failures.first().isEmpty());
    QCOMPARE(framesSeen, 0);
    QCOMPARE(starts, 0);
}

QTEST_GUILESS_MAIN(TestVideoEncode)
#include "tst_VideoEncode.moc"
