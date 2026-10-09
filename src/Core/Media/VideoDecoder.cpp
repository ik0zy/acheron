#include "Core/Media/VideoDecoder.hpp"

#include "Core/Logging.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <span>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace Acheron {
namespace Core {
namespace Media {

namespace {

using Discord::Voice::VideoCodec;

// ffmpeg's own AV1 decoder only works through a hardware accelerator, so software decoding
// needs one of the wrapped libraries.
const AVCodec *findSoftwareDecoder(VideoCodec codec)
{
    static constexpr std::array<const char *, 2> AV1 = { "libdav1d", "libaom-av1" };
    static constexpr std::array<const char *, 1> H265 = { "hevc" };
    static constexpr std::array<const char *, 1> H264 = { "h264" };
    static constexpr std::array<const char *, 2> VP8 = { "vp8", "libvpx" };

    std::span<const char *const> names;
    switch (codec) {
    case VideoCodec::AV1:
        names = AV1;
        break;
    case VideoCodec::H265:
        names = H265;
        break;
    case VideoCodec::H264:
        names = H264;
        break;
    case VideoCodec::VP8:
        names = VP8;
        break;
    case VideoCodec::Unknown:
        break;
    }

    for (const char *name : names) {
        if (const AVCodec *decoder = avcodec_find_decoder_by_name(name))
            return decoder;
    }
    return nullptr;
}

AVCodecContext *openDecoder(const AVCodec *decoder)
{
    AVCodecContext *context = avcodec_alloc_context3(decoder);
    if (!context)
        return nullptr;

    // Frame threading holds back one frame per thread, which a live stream cannot afford.
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_type = FF_THREAD_SLICE;
    context->thread_count = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
    av_opt_set_int(context, "max_frame_delay", 1, AV_OPT_SEARCH_CHILDREN);

    if (avcodec_open2(context, decoder, nullptr) < 0) {
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

} // namespace

QList<Discord::Voice::VideoCodec> decodableVideoCodecs()
{
    static const QList<VideoCodec> codecs = [] {
        QList<VideoCodec> found;
        for (VideoCodec codec : { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 }) {
            const AVCodec *decoder = findSoftwareDecoder(codec);
            AVCodecContext *context = decoder ? openDecoder(decoder) : nullptr;
            if (!context)
                continue;
            avcodec_free_context(&context);
            found.append(codec);
        }
        return found;
    }();
    return codecs;
}

QList<Discord::Voice::Codec> offeredVideoCodecs()
{
    static constexpr int FIRST_PAYLOAD_TYPE = 101;

    QList<Discord::Voice::Codec> offered;
    const QList<VideoCodec> codecs = decodableVideoCodecs();
    for (int i = 0; i < codecs.size(); i++) {
        Discord::Voice::Codec codec;
        codec.name = Discord::Voice::videoCodecName(codecs[i]);
        codec.type = QStringLiteral("video");
        codec.priority = (i + 1) * 1000;
        codec.payloadType = FIRST_PAYLOAD_TYPE + 2 * i;
        codec.rtxPayloadType = FIRST_PAYLOAD_TYPE + 2 * i + 1;
        codec.encode = false;
        codec.decode = true;
        offered.append(codec);
    }
    return offered;
}

VideoDecoder::VideoDecoder()
    : thread([this] { run(); })
{
}

VideoDecoder::~VideoDecoder()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    thread.join();
}

void VideoDecoder::setSink(std::shared_ptr<VideoFrameSink> sink)
{
    std::lock_guard lock(mutex);
    frameSink = std::move(sink);
}

void VideoDecoder::setKeyframeRequester(std::function<void()> requester)
{
    std::lock_guard lock(mutex);
    keyframeRequester = std::move(requester);
}

VideoDecoder::Stats VideoDecoder::stats() const
{
    std::lock_guard lock(mutex);
    return counters;
}

void VideoDecoder::push(Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame)
{
    bool overflowed = false;
    std::function<void()> requester;
    {
        std::lock_guard lock(mutex);
        if (static_cast<int>(queue.size()) >= MAX_QUEUED_FRAMES) {
            // Too far behind to catch up frame by frame.
            counters.dropped += queue.size();
            queue.clear();
            overflowed = true;
            requester = keyframeRequester;
        }
        queue.push_back({ codec, std::move(frame) });
    }
    wake.notify_one();

    if (overflowed && requester)
        requester();
}

void VideoDecoder::run()
{
    decoded = av_frame_alloc();

    for (;;) {
        Queued queued;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping)
                break;
            queued = std::move(queue.front());
            queue.pop_front();
        }
        decode(queued);
    }

    close();
    av_frame_free(&decoded);
}

bool VideoDecoder::open(Discord::Voice::VideoCodec codec)
{
    close();

    const AVCodec *decoder = findSoftwareDecoder(codec);
    context = decoder ? openDecoder(decoder) : nullptr;
    if (!context) {
        qCWarning(LogVideo) << "No decoder could be opened for" << Discord::Voice::videoCodecName(codec);
        return false;
    }

    openCodec = codec;
    awaitingKeyframe = true;

    std::lock_guard lock(mutex);
    counters.decoderName = QString::fromUtf8(decoder->name);
    return true;
}

void VideoDecoder::close()
{
    avcodec_free_context(&context);
    openCodec = Discord::Voice::VideoCodec::Unknown;
}

void VideoDecoder::fail()
{
    if (context)
        avcodec_flush_buffers(context);
    awaitingKeyframe = true;

    std::function<void()> requester;
    {
        std::lock_guard lock(mutex);
        counters.errors++;
        requester = keyframeRequester;
    }
    if (requester)
        requester();
}

void VideoDecoder::decode(Queued &queued)
{
    if (queued.codec != openCodec && !open(queued.codec))
        return;

    if (awaitingKeyframe && !queued.frame.keyframe) {
        std::lock_guard lock(mutex);
        counters.dropped++;
        return;
    }

    QElapsedTimer timer;
    timer.start();

    std::vector<uint8_t> &data = queued.frame.data;
    const int size = static_cast<int>(data.size());
    data.resize(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);

    AVPacket *packet = av_packet_alloc();
    packet->data = data.data();
    packet->size = size;
    packet->pts = queued.frame.rtpTimestamp;
    if (queued.frame.keyframe)
        packet->flags |= AV_PKT_FLAG_KEY;

    const int sent = avcodec_send_packet(context, packet);
    av_packet_free(&packet);
    if (sent < 0) {
        fail();
        return;
    }
    awaitingKeyframe = false;

    std::shared_ptr<VideoFrameSink> sink;
    {
        std::lock_guard lock(mutex);
        sink = frameSink;
    }

    for (;;) {
        const int received = avcodec_receive_frame(context, decoded);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
            break;
        if (received < 0) {
            fail();
            return;
        }

        if (sink)
            sink->present(decoded);

        {
            std::lock_guard lock(mutex);
            counters.decoded++;
            counters.width = decoded->width;
            counters.height = decoded->height;
            const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            counters.averageDecodeMs += (elapsedMs - counters.averageDecodeMs) / 30.0;
        }
        av_frame_unref(decoded);
    }
}

} // namespace Media
} // namespace Core
} // namespace Acheron
