#pragma once

#include <QSize>
#include <QString>

#include <chrono>
#include <optional>

#include "Core/Video/BroadcastPipeline.hpp"

struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {

// The size a source is sent at: fitted inside (maxHeight * 16 / 9) x maxHeight with its shape
// kept, never enlarged, and both sides even. A `maxHeight` of zero is the source's own size.
[[nodiscard]] QSize encodeSizeFor(const QSize &source, int maxHeight);

// Captures one screen at a steady rate, scaled and converted for an encoder. It scales on the
// graphics card when that works and in software when it does not, and builds its capture anew
// whenever Windows takes the screen away, which it does on a mode change, on a switch to the
// secure desktop, and when a full-screen program takes the output over.
//
// Not thread safe; it belongs to the thread that reads from it.
class ScreenCapture
{
public:
    struct Request
    {
        QString sourceId;
        int maxHeight = 0;
        int framerate = 30;
        // The format (an AVPixelFormat, NV12 or YUV420P) of pictures in system memory.
        int softwareFormat = -1;
        // Pictures are left on the graphics card as Direct3D 11 NV12 frames whenever they can
        // be scaled there, and arrive in `softwareFormat` otherwise.
        bool keepOnCard = false;
        // With `keepOnCard`: a screen sent at its own size is handed over as the BGRA frames
        // it is captured in, marked with the colours the encoder is to convert them to.
        bool rgbOnCard = false;
        // For a codec that cannot say which colours it carries and is taken as BT.601.
        bool bt601Only = false;
    };

    enum class Status {
        Frame,
        // Nothing new yet; ask again.
        NoFrame,
        // The screen cannot be captured at the moment; ask again. Each call waits a little.
        Stalled,
        Failed,
    };

    ScreenCapture() = default;
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;

    void configure(const Request &request);
    // Blocks until the next picture is due. A picture must be let go of before the next is
    // read: the pictures scaled on the card all share one texture.
    Status read(AVFrame *into);
    [[nodiscard]] QString failure() const { return failureText; }

    static constexpr int RETRY_INTERVAL_MS = 250;
    static constexpr int GIVE_UP_AFTER_MS = 5000;

private:
    enum class CardScaling {
        Untried,
        Works,
        // A capture that scaled on the card failed before its first picture, and one without
        // is being tried to see whether the scaling was at fault.
        Suspected,
    };

    int build();
    void tearDown();
    Status afterFailure(int error);

    Request request;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    bool graphScalesOnCard = false;
    bool graphIsBt601 = false;
    quint64 picturesFromGraph = 0;
    bool everDelivered = false;
    CardScaling cardScaling = CardScaling::Untried;
    std::optional<std::chrono::steady_clock::time_point> failingSince;
    std::chrono::steady_clock::time_point retryAt;
    bool failed = false;
    QString failureText;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
