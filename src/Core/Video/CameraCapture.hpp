#pragma once

#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {

// Where the camera pipeline's pictures come from: a camera, or in a test whatever stands in
// for one. It belongs to the thread that reads it.
class CameraFrameSource
{
public:
    enum class Status {
        Frame,
        NoFrame,
        // Nothing more will come, and failure() says why.
        Ended,
    };

    virtual ~CameraFrameSource() = default;

    // An empty id takes the first camera there is. False when it cannot be had, and failure()
    // says why.
    [[nodiscard]] virtual bool open(const QString &deviceId) = 0;
    // The newest picture not handed out yet, waiting a moment for one. `into` is empty, and is
    // given a picture in system memory in whatever format the camera has, marked with its own
    // colours. A picture nobody read before the next one came is gone: a reader that is slow
    // sees fewer pictures and never older ones. `capturedAt` only ever grows.
    [[nodiscard]] virtual Status read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) = 0;
    [[nodiscard]] virtual QString failure() const = 0;
    // What the pictures come from and in which mode, for the statistics.
    [[nodiscard]] virtual QString description() const = 0;
};

using CameraFrameSourceFactory = std::function<std::unique_ptr<CameraFrameSource>()>;

// Tests put a source of their own in the camera's place. An empty factory brings the camera back.
void setCameraFrameSourceFactory(CameraFrameSourceFactory factory);
// Null where this build cannot open cameras.
[[nodiscard]] std::unique_ptr<CameraFrameSource> makeCameraFrameSource();

// When each of a camera's pictures was taken, by this program's clock. A camera stamps its
// pictures by a clock of its own, which says how far apart they were taken more truly than the
// moments at which they were read here do: a picture that waited to be read was taken no later
// for it. The shortest time a picture has taken to get here ties the one clock to the other.
class CaptureClock
{
public:
    // `stamped` is the camera's own time for the picture, where it gave one. What comes back is
    // not after `readAt`, unless it has to be to come after the picture before.
    [[nodiscard]] std::chrono::steady_clock::time_point at(std::optional<std::chrono::microseconds> stamped, std::chrono::steady_clock::time_point readAt);

    // A stamp this far behind where the last ones were is the camera's clock starting over.
    static constexpr std::chrono::milliseconds STARTED_OVER{ 1000 };
    // A camera's clock that loses up to one part in this many against ours is followed as it
    // falls behind, and its pictures are not taken to be older and older.
    static constexpr int SLOWEST_CLOCK_FOLLOWED = 5000;

private:
    std::optional<std::chrono::steady_clock::duration> ourClockAhead;
    std::chrono::steady_clock::time_point lastReadAt;
    std::optional<std::chrono::steady_clock::time_point> last;
};

#if defined(ACHERON_HAVE_AVDEVICE) && defined(Q_OS_WIN)

// A camera, opened through ffmpeg's DirectShow input. It is asked for 1280x720 at 30 as MJPEG,
// which is the only way many USB cameras deliver that, then for the same in any format, then
// for 640x480 at 30, and at last for whatever it gives when asked for nothing.
//
// A thread of its own opens the camera, reads it and decodes what it sends, so that nothing
// waits in the device's buffer for a reader that is busy encoding.
class CameraCapture : public CameraFrameSource
{
public:
    CameraCapture();
    ~CameraCapture() override;

    CameraCapture(const CameraCapture &) = delete;
    CameraCapture &operator=(const CameraCapture &) = delete;

    [[nodiscard]] bool open(const QString &deviceId) override;
    [[nodiscard]] Status read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) override;
    [[nodiscard]] QString failure() const override;
    [[nodiscard]] QString description() const override;

    static constexpr int READ_WAIT_MS = 100;
    // A camera that opens and then sends nothing for this long is taken to be lost. One that
    // another program holds can open like that.
    static constexpr int NO_PICTURE_TIMEOUT_MS = 5000;

private:
    enum class State {
        Opening,
        Open,
        Ended,
    };

    void run(const QString &deviceId);
    void opened(const QString &mode);
    void end(const QString &reason);
    void publish(AVFrame *picture, std::chrono::steady_clock::time_point capturedAt);

    mutable std::mutex mutex;
    std::condition_variable changed;
    State state = State::Opening;
    QString failureText;
    QString mode;
    AVFrame *newest;
    std::chrono::steady_clock::time_point newestCapturedAt;

    std::atomic<bool> stopping{ false };
    std::thread thread;
};

#endif

} // namespace Video
} // namespace Core
} // namespace Acheron
