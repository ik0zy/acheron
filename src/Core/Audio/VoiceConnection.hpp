#pragma once

#include <QList>
#include <QObject>
#include <QThread>

#include <functional>

#include "Discord/Voice/VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {
class VoiceClient;
}
} // namespace Discord

namespace Core {
namespace Audio {

// One RTC connection and the thread it runs on. The voice connection is one of these, and so
// is each Go Live stream connection.
class VoiceConnection : public QObject
{
    Q_OBJECT
public:
    explicit VoiceConnection(const Discord::Voice::ConnectionParams &params, QObject *parent = nullptr);
    ~VoiceConnection() override;

    [[nodiscard]] Discord::Voice::VoiceClient *client() const { return voiceClient; }
    [[nodiscard]] QThread *networkThread() const { return connectionThread; }
    [[nodiscard]] bool isRunning() const { return connectionThread->isRunning(); }

    // Objects that have to live on the connection's thread while it runs. They come back to
    // this object's thread when the connection stops.
    void adopt(QObject *object);

    // Refuses, before any socket opens, when the account's proxy cannot carry UDP: every RTC
    // connection sends its media over UDP, and falling back to a direct route would leak it.
    bool start();

    // `beforeStop` runs on the connection's thread ahead of the client being stopped.
    void stop(const std::function<void()> &beforeStop = {});

private:
    Discord::Voice::ConnectionParams params;
    Discord::Voice::VoiceClient *voiceClient;
    QThread *connectionThread;
    QList<QObject *> adopted;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
