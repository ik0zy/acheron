#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

#include "Core/Video/BroadcastPipeline.hpp"
#include "Discord/Voice/VoiceEnums.hpp"

namespace Acheron {
namespace Core {
namespace Video {

struct CameraDevice
{
    // Stays the same for a camera from one run to the next, and is what settings remember.
    QString id;
    QString name;
};

// False where this build cannot open cameras at all, in which case nothing offers one.
[[nodiscard]] bool cameraCaptureAvailable();
// The cameras attached now. Asking is cheap and opens none of them.
[[nodiscard]] QList<CameraDevice> cameraDevices();

struct CameraSettings
{
    // Empty takes the first camera there is.
    QString deviceId;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    // What is encoded. The camera itself is always asked for 1280x720 at 30, or the nearest it
    // has, and its picture is scaled and its frames thinned to this.
    QSize encodeSize = QSize(1280, 720);
    int framerate = 30;
    // Bits per second.
    int bitrate = 0;
    int maxBitrate = 0;
    // From the server; zero means key frames are made only when asked for.
    int keyframeIntervalMs = 0;
};

// Opens a camera and encodes its picture, on a thread of its own, with the same manners as
// BroadcastPipeline: its signals are emitted from that thread, and encoded frames are whole
// access units with a 90 kHz timestamp, in the shapes described there.
class CameraPipeline : public QObject
{
    Q_OBJECT
public:
    explicit CameraPipeline(QObject *parent = nullptr);
    ~CameraPipeline() override;

    // The camera's pictures can be encoded more than once, each layer at a size, frame rate
    // and bitrate of its own with an encoder of its own. Layer 0 is the one the settings
    // describe. The others encode nothing until they are taken out of suspension, and start
    // as layer 0 does unless they are told otherwise first.
    static constexpr int LAYERS = 2;

    // All of these may be called from any thread and return at once.
    void start(const CameraSettings &settings);
    void stop();
    // The first frame the layer encodes after this is a key frame.
    void requestKeyframe(int layer = 0);
    // Encoders that cannot follow a new rate while running are reopened, which costs a key
    // frame, so callers should move this in steps and not continuously.
    void setBitrate(int bitrate, int layer = 0);
    // A new size or frame rate to encode at, the camera staying open. Starts with a key frame.
    void setEncoding(const QSize &encodeSize, int framerate, int layer = 0);
    // While nobody is watching: the camera stays open, so the preview stays live, and the
    // layer encodes nothing. Sending again starts with a key frame.
    void setEncodingSuspended(bool suspended, int layer = 0);

signals:
    void started(const QSize &encodedSize, int framerate, int layer);
    void frameEncoded(const Acheron::Core::Video::EncodedFramePointer &frame, int layer);
    // The camera's own picture for its owner to look at: no larger than 640x360, about fifteen
    // times a second, not mirrored.
    void previewReady(const QImage &preview);
    // Nothing more will come: the camera is gone or in use elsewhere, or no encoder opened.
    void failed(const QString &reason);
    void statsUpdated(const Acheron::Core::Video::BroadcastStats &stats, int layer);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
