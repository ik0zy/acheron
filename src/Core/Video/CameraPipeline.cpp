#include "Core/Video/CameraPipeline.hpp"

#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/CameraCapture.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace Core {
namespace Video {

namespace {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;
using Clock = std::chrono::steady_clock;

constexpr int PREVIEWS_PER_SECOND = 15;
constexpr QSize PREVIEW_BOUND(640, 360);
constexpr int STATS_INTERVAL_MS = 1000;
constexpr int HD_HEIGHT = 720;
constexpr int MIN_BITRATE = 50'000;
constexpr int BITRATE_WHEN_NONE_GIVEN = 2'500'000;

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

struct Encoding
{
    QSize size;
    int framerate = 30;
};

struct Asked
{
    int bitrate = 0;
    // Set when a size and frame rate were asked for, also the ones there already are.
    std::optional<Encoding> encoding;
};

int usableBitrate(int wanted, int most)
{
    if (wanted <= 0)
        wanted = BITRATE_WHEN_NONE_GIVEN;
    if (most > 0)
        wanted = std::min(wanted, most);
    return std::max(wanted, MIN_BITRATE);
}

// The camera's picture inside the size to encode: never stretched and never enlarged, so a
// camera of another shape is sent smaller than asked, and both sides even.
QSize fittedInside(const QSize &camera, const QSize &bound)
{
    QSize size = camera;
    if (!bound.isEmpty() && (camera.width() > bound.width() || camera.height() > bound.height()))
        size = camera.scaled(bound, Qt::KeepAspectRatio);
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

// Lets through as many pictures a second as are wanted, of a camera that takes more.
class FrameThinner
{
public:
    explicit FrameThinner(int framerate) : interval(std::chrono::microseconds(1'000'000 / std::max(1, framerate))) {}

    bool takes(Clock::time_point capturedAt)
    {
        // A camera's pictures come a little early as often as a little late.
        if (due && capturedAt < *due - interval / 4)
            return false;
        due = std::max(due ? *due + interval : capturedAt + interval, capturedAt);
        return true;
    }

private:
    Clock::duration interval;
    std::optional<Clock::time_point> due;
};

// Makes of the camera's picture the one that is encoded: at the size to send, in the format
// the encoder takes, and in the colours a viewer takes a stream to be in when it does not say:
// video range, where a camera that sends MJPEG sends full range, and BT.709 from 720 lines up
// and BT.601 below. The picture says so as well, but not every encoder passes that on (AMF
// leaves BT.601 unsaid), and VP8 has no way to: it is BT.601 by definition.
class EncodePictureMaker
{
public:
    EncodePictureMaker() = default;
    ~EncodePictureMaker() { sws_freeContext(context); }

    EncodePictureMaker(const EncodePictureMaker &) = delete;
    EncodePictureMaker &operator=(const EncodePictureMaker &) = delete;

    // Null when the picture could not be converted. `camera` itself when it is already as wanted.
    AVFrame *make(AVFrame *camera, const QSize &size, int format, bool bt601Only)
    {
        const Conversion wanted{ camera->width, camera->height, camera->format, Media::usesBt709(camera), Media::isFullRange(camera), size, format, !bt601Only && size.height() >= HD_HEIGHT };
        if (wanted.changesNothing())
            return camera;
        if (wanted != configuredFor && !configure(wanted))
            return nullptr;

        if (made->width != size.width() || made->height != size.height() || made->format != format) {
            av_frame_unref(made.get());
            made->width = size.width();
            made->height = size.height();
            made->format = format;
            if (av_frame_get_buffer(made.get(), 0) < 0)
                return nullptr;
        }
        // An encoder may still hold the last picture.
        if (av_frame_make_writable(made.get()) < 0)
            return nullptr;
        if (sws_scale(context, camera->data, camera->linesize, 0, camera->height, made->data, made->linesize) <= 0)
            return nullptr;

        made->colorspace = wanted.toBt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        made->color_range = AVCOL_RANGE_MPEG;
        made->color_primaries = camera->color_primaries;
        made->color_trc = camera->color_trc;
        return made.get();
    }

private:
    struct Conversion
    {
        int width = 0;
        int height = 0;
        int format = -1;
        bool fromBt709 = false;
        bool fromFullRange = false;
        QSize size;
        int toFormat = -1;
        bool toBt709 = false;

        bool operator==(const Conversion &) const = default;
        [[nodiscard]] bool changesNothing() const { return size == QSize(width, height) && toFormat == format && toBt709 == fromBt709 && !fromFullRange; }
    };

    bool configure(const Conversion &wanted)
    {
        sws_freeContext(context);
        context = sws_alloc_context();
        if (!context)
            return false;
        configuredFor = {};

        // The ranges are set before the context is made ready: swscale picks a plain copy for
        // two formats that differ only in their layout, and that does not convert a range it
        // is told of afterwards.
        av_opt_set_int(context, "srcw", wanted.width, 0);
        av_opt_set_int(context, "srch", wanted.height, 0);
        av_opt_set_int(context, "src_format", wanted.format, 0);
        av_opt_set_int(context, "src_range", wanted.fromFullRange ? 1 : 0, 0);
        av_opt_set_int(context, "dstw", wanted.size.width(), 0);
        av_opt_set_int(context, "dsth", wanted.size.height(), 0);
        av_opt_set_int(context, "dst_format", wanted.toFormat, 0);
        av_opt_set_int(context, "dst_range", 0, 0);
        av_opt_set_int(context, "sws_flags", SWS_BILINEAR, 0);
        if (sws_init_context(context, nullptr, nullptr) < 0)
            return false;

        sws_setColorspaceDetails(context, sws_getCoefficients(wanted.fromBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), wanted.fromFullRange ? 1 : 0, sws_getCoefficients(wanted.toBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), 0, 0, 1 << 16, 1 << 16);
        configuredFor = wanted;
        return true;
    }

    SwsContext *context = nullptr;
    Conversion configuredFor;
    FramePointer made{ av_frame_alloc() };
};

// The camera's picture for its owner to look at, each in an image of its own.
class PreviewMaker
{
public:
    QImage make(const AVFrame *camera)
    {
        const QSize full(camera->width, camera->height);
        const bool fits = full.width() <= PREVIEW_BOUND.width() && full.height() <= PREVIEW_BOUND.height();
        const QSize size = fits ? full : full.scaled(PREVIEW_BOUND, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        if (image.size() != size)
            image = Media::RgbScaler::allocate(size, QImage::Format_RGB32);
        return scaler.scale(camera, image) ? image : QImage();
    }

private:
    Media::RgbScaler scaler;
    QImage image;
};

Media::VideoEncoder::Settings encoderSettings(const CameraSettings &settings, int framerate, int bitrate)
{
    Media::VideoEncoder::Settings encoding;
    encoding.codec = settings.codec;
    encoding.framerate = framerate;
    encoding.bitrate = bitrate;
    encoding.keyframeIntervalMs = settings.keyframeIntervalMs;
    encoding.content = Media::VideoEncoder::Content::Camera;
    return encoding;
}

} // namespace

class CameraPipeline::Worker
{
public:
    explicit Worker(CameraPipeline *pipeline) : pipeline(pipeline), thread([this] { run(); }) {}

    ~Worker()
    {
        {
            std::lock_guard lock(mutex);
            quitting = true;
            generation++;
        }
        wake.notify_all();
        thread.join();
    }

    void start(const CameraSettings &settings)
    {
        {
            std::lock_guard lock(mutex);
            wanted = settings;
            encodingWanted.reset();
            generation++;
        }
        wake.notify_all();
    }

    void stop()
    {
        std::lock_guard lock(mutex);
        wanted.reset();
        generation++;
    }

    // A camera that has not been opened yet opens with what is asked for here.
    void setBitrate(int newBitrate)
    {
        std::lock_guard lock(mutex);
        if (wanted) {
            wanted->bitrate = newBitrate;
        } else if (bitrate != newBitrate) {
            bitrate = newBitrate;
            changes++;
        }
    }

    void setEncoding(const QSize &size, int framerate)
    {
        std::lock_guard lock(mutex);
        if (wanted) {
            wanted->encodeSize = size;
            wanted->framerate = framerate;
        } else {
            encodingWanted = Encoding{ size, std::max(1, framerate) };
            changes++;
        }
    }

    std::atomic<bool> keyframeWanted{ false };
    std::atomic<bool> suspended{ false };

private:
    void run();
    void capture(const CameraSettings &settings, quint64 session);
    [[nodiscard]] std::optional<Asked> takeAsked();
    [[nodiscard]] int64_t timestampOf(Clock::time_point capturedAt);

    CameraPipeline *pipeline;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<CameraSettings> wanted;
    int bitrate = 0;
    std::optional<Encoding> encodingWanted;
    quint64 changes = 0;
    quint64 changesAtLastPicture = 0;
    bool quitting = false;
    // Moves on with every start and stop; a capture ends when it is no longer its own.
    std::atomic<quint64> generation{ 0 };

    // Timestamps run on from one capture to the next, so that a restarted one never goes back.
    const Clock::time_point origin = Clock::now();
    int64_t lastTimestamp = -1;

    std::thread thread;
};

void CameraPipeline::Worker::run()
{
    for (;;) {
        CameraSettings settings;
        quint64 session = 0;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return quitting || wanted; });
            if (quitting)
                return;
            settings = *wanted;
            wanted.reset();
            bitrate = settings.bitrate;
            session = generation;
        }
        capture(settings, session);
    }
}

// What is asked for now, once a picture has come with nothing new asked for since the picture
// before it. A new bit rate and a new size asked for one after the other are each a new start
// for the encoder, and a key frame: taken together like this they are one.
std::optional<Asked> CameraPipeline::Worker::takeAsked()
{
    std::lock_guard lock(mutex);
    if (changes != std::exchange(changesAtLastPicture, changes))
        return std::nullopt;
    return Asked{ bitrate, std::exchange(encodingWanted, std::nullopt) };
}

int64_t CameraPipeline::Worker::timestampOf(Clock::time_point capturedAt)
{
    const int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(capturedAt - origin).count();
    lastTimestamp = std::max(lastTimestamp + 1, microseconds * 9 / 100);
    return lastTimestamp;
}

void CameraPipeline::Worker::capture(const CameraSettings &settings, quint64 session)
{
    const QString noEncoder = CameraPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(settings.codec));
    Encoding encoding{ settings.encodeSize, std::max(1, settings.framerate) };
    int sendingBitrate = usableBitrate(settings.bitrate, settings.maxBitrate);
    std::optional<Media::VideoEncoder> encoder(std::in_place, encoderSettings(settings, encoding.framerate, sendingBitrate));
    if (!encoder->hasEncoder()) {
        emit pipeline->failed(noEncoder);
        return;
    }

    const std::unique_ptr<CameraFrameSource> camera = makeCameraFrameSource();
    if (!camera || !camera->open(settings.deviceId)) {
        emit pipeline->failed(camera ? camera->failure() : CameraPipeline::tr("This build cannot open cameras."));
        return;
    }

    const FramePointer frame(av_frame_alloc());
    EncodePictureMaker pictures;
    PreviewMaker previews;
    FrameThinner encodeThinner(encoding.framerate);
    FrameThinner previewThinner(PREVIEWS_PER_SECOND);
    std::vector<EncodedVideoFrame> encoded;
    QElapsedTimer sinceStats;
    sinceStats.start();

    std::optional<Encoding> announced;
    bool wasSuspended = false;
    quint64 framesEncoded = 0;
    quint64 framesDropped = 0;
    int capturedLately = 0;
    int encodedLately = 0;
    quint64 bytesLately = 0;

    while (generation == session) {
        if (sinceStats.elapsed() >= STATS_INTERVAL_MS) {
            const double seconds = static_cast<double>(sinceStats.restart()) / 1000.0;
            const Media::VideoEncoder::Stats &encoderStats = encoder->stats();
            BroadcastStats stats;
            // The camera's mode goes into the name for want of a place of its own in the statistics.
            stats.encoderName = QStringLiteral("%1, from %2").arg(encoderStats.encoderName.isEmpty() ? QStringLiteral("no encoder yet") : encoderStats.encoderName, camera->description());
            stats.hardware = encoderStats.hardware;
            stats.encodedSize = announced ? announced->size : QSize();
            stats.capturedFps = capturedLately / seconds;
            stats.encodedFps = encodedLately / seconds;
            stats.averageEncodeMs = encoderStats.averageEncodeMs;
            stats.bitrateBps = static_cast<quint64>(static_cast<double>(bytesLately) * 8.0 / seconds);
            stats.framesEncoded = framesEncoded;
            stats.framesDropped = framesDropped;
            emit pipeline->statsUpdated(stats);
            capturedLately = 0;
            encodedLately = 0;
            bytesLately = 0;
        }

        av_frame_unref(frame.get());
        Clock::time_point capturedAt;
        const CameraFrameSource::Status status = camera->read(frame.get(), capturedAt);
        if (status == CameraFrameSource::Status::Ended) {
            emit pipeline->failed(camera->failure());
            return;
        }
        if (status == CameraFrameSource::Status::NoFrame)
            continue;
        capturedLately++;

        if (previewThinner.takes(capturedAt)) {
            const QImage preview = previews.make(frame.get());
            if (!preview.isNull())
                emit pipeline->previewReady(preview);
        }

        const std::optional<Asked> asked = takeAsked();
        if (asked)
            sendingBitrate = usableBitrate(asked->bitrate, settings.maxBitrate);
        // VideoEncoder has no way to be given a new frame rate, so a new one is made, which
        // starts with a key frame as a new size would anyway.
        if (asked && asked->encoding) {
            encoding = *asked->encoding;
            encoder.emplace(encoderSettings(settings, encoding.framerate, sendingBitrate));
            encodeThinner = FrameThinner(encoding.framerate);
        }
        // An encoder that has yet to open waits for what is still being asked for, and opens once.
        if (!asked && encoder->stats().opened == 0)
            continue;
        const Encoding sending{ fittedInside(QSize(frame->width, frame->height), encoding.size), encoding.framerate };
        const auto announce = [this, &announced, &sending] {
            if (!announced || announced->size != sending.size || announced->framerate != sending.framerate)
                emit pipeline->started(sending.size, sending.framerate);
            announced = sending;
        };

        if (suspended) {
            wasSuspended = true;
            keyframeWanted = false;
            announce();
            continue;
        }
        if (!encodeThinner.takes(capturedAt))
            continue;

        AVFrame *picture = pictures.make(frame.get(), sending.size, encoder->softwareFormat(), settings.codec == VideoCodec::VP8);
        if (!picture) {
            emit pipeline->failed(CameraPipeline::tr("The camera's picture could not be converted."));
            return;
        }

        encoder->setBitrate(sendingBitrate);
        if (keyframeWanted.exchange(false) || std::exchange(wasSuspended, false))
            encoder->requestKeyframe();
        encoded.clear();
        if (encoder->encode(picture, timestampOf(capturedAt), encoded) != Media::VideoEncoder::Outcome::Encoded) {
            emit pipeline->failed(noEncoder);
            return;
        }
        announce();

        framesDropped += encoded.empty() ? 1 : 0;
        for (EncodedVideoFrame &each : encoded) {
            framesEncoded++;
            encodedLately++;
            bytesLately += each.data.size();
            emit pipeline->frameEncoded(std::make_shared<const EncodedVideoFrame>(std::move(each)));
        }
    }
}

CameraPipeline::CameraPipeline(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<EncodedFramePointer>("Acheron::Core::Video::EncodedFramePointer");
    qRegisterMetaType<BroadcastStats>("Acheron::Core::Video::BroadcastStats");
    worker = std::make_unique<Worker>(this);
}

CameraPipeline::~CameraPipeline() = default;

void CameraPipeline::start(const CameraSettings &settings)
{
    worker->start(settings);
}

void CameraPipeline::stop()
{
    worker->stop();
}

void CameraPipeline::requestKeyframe()
{
    worker->keyframeWanted = true;
}

void CameraPipeline::setBitrate(int bitrate)
{
    worker->setBitrate(bitrate);
}

void CameraPipeline::setEncoding(const QSize &encodeSize, int framerate)
{
    worker->setEncoding(encodeSize, framerate);
}

void CameraPipeline::setEncodingSuspended(bool suspended)
{
    worker->suspended = suspended;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
