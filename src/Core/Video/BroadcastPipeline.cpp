#include "Core/Video/BroadcastPipeline.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/ScreenCapture.hpp"

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
#include <libavutil/hwcontext.h>
}

namespace Acheron {
namespace Core {
namespace Video {

namespace {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

constexpr int PREVIEW_INTERVAL_MS = 500;
constexpr QSize PREVIEW_BOUND(512, 288);
constexpr int STATS_INTERVAL_MS = 1000;
constexpr int MIN_BITRATE = 50'000;
constexpr int BITRATE_WHEN_NONE_GIVEN = 2'500'000;

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

int usableBitrate(int wanted, int most)
{
    if (wanted <= 0)
        wanted = BITRATE_WHEN_NONE_GIVEN;
    if (most > 0)
        wanted = std::min(wanted, most);
    return std::max(wanted, MIN_BITRATE);
}

// Small stills of what is being captured, each in an image of its own.
class PreviewMaker
{
public:
    QImage make(const AVFrame *frame)
    {
        const AVFrame *picture = frame;
        if (frame->hw_frames_ctx) {
            av_frame_unref(downloaded.get());
            if (av_hwframe_transfer_data(downloaded.get(), frame, 0) < 0)
                return {};
            av_frame_copy_props(downloaded.get(), frame);
            picture = downloaded.get();
        }

        const QSize full(picture->width, picture->height);
        const bool fits = full.width() <= PREVIEW_BOUND.width() && full.height() <= PREVIEW_BOUND.height();
        const QSize size = fits ? full : full.scaled(PREVIEW_BOUND, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        if (image.size() != size)
            image = Media::RgbScaler::allocate(size, QImage::Format_RGB32);
        return scaler.scale(picture, image) ? image : QImage();
    }

private:
    Media::RgbScaler scaler;
    QImage image;
    FramePointer downloaded{ av_frame_alloc() };
};

} // namespace

class BroadcastPipeline::Worker
{
public:
    explicit Worker(BroadcastPipeline *pipeline) : pipeline(pipeline), thread([this] { run(); }) {}

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

    void start(const BroadcastSettings &settings)
    {
        {
            std::lock_guard lock(mutex);
            wanted = settings;
            bitrate = settings.bitrate;
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

    std::atomic<bool> keyframeWanted{ false };
    std::atomic<bool> suspended{ false };
    std::atomic<int> bitrate{ 0 };

private:
    void run();
    void capture(const BroadcastSettings &settings, quint64 session);
    [[nodiscard]] int64_t nextTimestamp();

    BroadcastPipeline *pipeline;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<BroadcastSettings> wanted;
    bool quitting = false;
    // Moves on with every start and stop; a capture ends when it is no longer its own.
    std::atomic<quint64> generation{ 0 };

    // Timestamps run on from one capture to the next, so that a restarted one never goes back.
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    int64_t lastTimestamp = -1;

    std::thread thread;
};

void BroadcastPipeline::Worker::run()
{
    for (;;) {
        BroadcastSettings settings;
        quint64 session = 0;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return quitting || wanted; });
            if (quitting)
                return;
            settings = *wanted;
            wanted.reset();
            session = generation;
        }
        capture(settings, session);
    }
}

int64_t BroadcastPipeline::Worker::nextTimestamp()
{
    const int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin).count();
    lastTimestamp = std::max(lastTimestamp + 1, microseconds * 9 / 100);
    return lastTimestamp;
}

void BroadcastPipeline::Worker::capture(const BroadcastSettings &settings, quint64 session)
{
    Media::VideoEncoder::Settings encoding;
    encoding.codec = settings.codec;
    encoding.framerate = std::max(1, settings.framerate);
    encoding.bitrate = usableBitrate(bitrate, settings.maxBitrate);
    encoding.keyframeIntervalMs = settings.keyframeIntervalMs;
    Media::VideoEncoder encoder(encoding);
    if (!encoder.hasEncoder()) {
        emit pipeline->failed(BroadcastPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(settings.codec)));
        return;
    }

    ScreenCapture::Request request;
    request.sourceId = settings.sourceId;
    request.maxHeight = settings.maxHeight;
    request.framerate = encoding.framerate;
    request.softwareFormat = encoder.softwareFormat();
    request.keepOnCard = encoder.takesCardFrames();
    request.rgbOnCard = encoder.takesRgbCardFrames();
    request.bt601Only = settings.codec == VideoCodec::VP8;
    ScreenCapture screen;
    screen.configure(request);

    const FramePointer frame(av_frame_alloc());
    PreviewMaker previews;
    std::vector<EncodedVideoFrame> encoded;
    QElapsedTimer sincePreview;
    QElapsedTimer sinceStats;
    sinceStats.start();

    bool announced = false;
    bool stalled = false;
    bool wasSuspended = false;
    quint64 framesEncoded = 0;
    quint64 framesDropped = 0;
    int capturedLately = 0;
    int encodedLately = 0;
    quint64 bytesLately = 0;

    while (generation == session) {
        const ScreenCapture::Status status = screen.read(frame.get());
        if (status == ScreenCapture::Status::Failed) {
            emit pipeline->failed(screen.failure());
            return;
        }
        if (status == ScreenCapture::Status::Stalled && announced && !stalled) {
            stalled = true;
            emit pipeline->captureStalled(true);
        }
        if (status != ScreenCapture::Status::Frame)
            continue;
        if (std::exchange(stalled, false))
            emit pipeline->captureStalled(false);

        const int64_t timestamp = nextTimestamp();
        const QSize size(frame->width, frame->height);
        capturedLately++;

        const bool keyframeAsked = keyframeWanted.exchange(false);
        const bool suspendedNow = suspended;
        if (!suspendedNow) {
            encoder.setBitrate(usableBitrate(bitrate, settings.maxBitrate));
            if (keyframeAsked || wasSuspended)
                encoder.requestKeyframe();

            encoded.clear();
            const Media::VideoEncoder::Outcome outcome = encoder.encode(frame.get(), timestamp, encoded);
            if (outcome == Media::VideoEncoder::Outcome::CardFramesRefused) {
                qCInfo(LogVideo) << "The encoder does not take pictures that are on the graphics card; reading them back into memory";
                request.keepOnCard = false;
                screen.configure(request);
                av_frame_unref(frame.get());
                continue;
            }
            if (outcome == Media::VideoEncoder::Outcome::Failed) {
                emit pipeline->failed(BroadcastPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(settings.codec)));
                return;
            }
        }
        wasSuspended = suspendedNow;

        if (!std::exchange(announced, true))
            emit pipeline->started(size, encoding.framerate);

        if (!suspendedNow) {
            framesDropped += encoded.empty() ? 1 : 0;
            for (EncodedVideoFrame &each : encoded) {
                framesEncoded++;
                encodedLately++;
                bytesLately += each.data.size();
                emit pipeline->frameEncoded(std::make_shared<const EncodedVideoFrame>(std::move(each)));
            }
        }

        if (!sincePreview.isValid() || sincePreview.elapsed() >= PREVIEW_INTERVAL_MS) {
            sincePreview.start();
            const QImage preview = previews.make(frame.get());
            if (!preview.isNull())
                emit pipeline->previewReady(preview);
        }

        if (sinceStats.elapsed() >= STATS_INTERVAL_MS) {
            const double seconds = static_cast<double>(sinceStats.restart()) / 1000.0;
            const Media::VideoEncoder::Stats &encoderStats = encoder.stats();
            BroadcastStats stats;
            stats.encoderName = encoderStats.encoderName;
            stats.hardware = encoderStats.hardware;
            stats.encodedSize = size;
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
    }
}

BroadcastPipeline::BroadcastPipeline(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<EncodedFramePointer>("Acheron::Core::Video::EncodedFramePointer");
    qRegisterMetaType<BroadcastStats>("Acheron::Core::Video::BroadcastStats");
    worker = std::make_unique<Worker>(this);
}

BroadcastPipeline::~BroadcastPipeline() = default;

QList<Discord::Voice::VideoCodec> BroadcastPipeline::encodableCodecs()
{
    return Media::encodableVideoCodecs();
}

void BroadcastPipeline::start(const BroadcastSettings &settings)
{
    worker->start(settings);
}

void BroadcastPipeline::stop()
{
    worker->stop();
}

void BroadcastPipeline::requestKeyframe()
{
    worker->keyframeWanted = true;
}

void BroadcastPipeline::setBitrate(int bitrate)
{
    worker->bitrate = bitrate;
}

void BroadcastPipeline::setEncodingSuspended(bool suspended)
{
    worker->suspended = suspended;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
