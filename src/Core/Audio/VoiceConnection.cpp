#include "VoiceConnection.hpp"

#include "Core/Logging.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {
namespace Audio {

VoiceConnection::VoiceConnection(const Discord::Voice::ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params), voiceClient(new Discord::Voice::VoiceClient(params)), connectionThread(new QThread(this))
{
    connectionThread->setObjectName("VoiceThread");
}

VoiceConnection::~VoiceConnection()
{
    stop();
    delete voiceClient;
}

void VoiceConnection::adopt(QObject *object)
{
    adopted.append(object);
}

bool VoiceConnection::start()
{
    if (!params.proxy.canRelayUdp()) {
        qCWarning(LogVoice) << "Refusing RTC connection: an HTTP proxy cannot relay its UDP";
        return false;
    }

    voiceClient->moveToThread(connectionThread);
    for (QObject *object : adopted)
        object->moveToThread(connectionThread);

    connectionThread->start();
    QMetaObject::invokeMethod(voiceClient, &Discord::Voice::VoiceClient::start);
    return true;
}

void VoiceConnection::stop(const std::function<void()> &beforeStop)
{
    if (!connectionThread->isRunning())
        return;

    QThread *home = QObject::thread();
    QMetaObject::invokeMethod(
            voiceClient,
            [this, &beforeStop, home]() {
                if (beforeStop)
                    beforeStop();
                voiceClient->stop();
                voiceClient->moveToThread(home);
                for (QObject *object : adopted)
                    object->moveToThread(home);
            },
            Qt::BlockingQueuedConnection);

    connectionThread->quit();
    if (!connectionThread->wait(5000)) {
        qCWarning(LogVoice) << "Voice thread did not stop in time, terminating";
        connectionThread->terminate();
        connectionThread->wait();
    }
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
