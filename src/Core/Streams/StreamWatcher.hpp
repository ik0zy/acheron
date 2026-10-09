#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

#include <optional>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamManager.hpp"
#include "Core/Streams/StreamSession.hpp"
#include "Discord/StreamKey.hpp"

namespace Acheron {
namespace Discord {
class Client;
}

namespace Core {

namespace Audio {
class VoiceManager;
}

// Starts and stops watching other people's streams, following the official client's rules for
// when watching one stream ends another.
class StreamWatcher : public QObject
{
    Q_OBJECT
public:
    StreamWatcher(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, Snowflake accountId, const ProxyConfig &proxy, QObject *parent = nullptr);

    void setGatewaySessionId(const QString &sessionId);

    // A stream can be watched from the voice channel it is in, by anyone but its owner.
    [[nodiscard]] bool canWatch(const Discord::StreamKey &key) const;
    void watch(const Discord::StreamKey &key);
    void stopWatching(const QString &streamKey);
    void stopAll();

    // A new gateway session has forgotten every stream, so nothing is sent for them.
    void reset();

    [[nodiscard]] StreamSession *session(const QString &streamKey) const;
    [[nodiscard]] QList<StreamSession *> openSessions() const { return sessions.values(); }

    // Watches the stream as soon as this account is in its channel, for when joining the
    // channel and watching are asked for in one gesture.
    void watchWhenConnected(const Discord::StreamKey &key);

signals:
    // Also sent for a stream already being watched, when it is asked for again.
    void sessionOpened(Acheron::Core::StreamSession *session);
    void sessionClosed(const QString &streamKey);

private slots:
    void onStreamChanged(const QString &streamKey);
    void onStreamEnded(const QString &streamKey, Acheron::Core::StreamEndReason reason);
    void onVoiceStateChanged();
    void autoWatch();

private:
    void remove(const QString &streamKey, bool tellGateway);

    Discord::Client *client;
    StreamManager *streams;
    Audio::VoiceManager *voice;
    Snowflake accountId;
    ProxyConfig proxy;
    QString gatewaySessionId;

    QHash<QString, StreamSession *> sessions;
    std::optional<Discord::StreamKey> pendingWatch;
    // Streams that turned this account away for being full; not asked for again on its behalf.
    QSet<QString> fullStreams;
    quint32 nextAudioSource = 1;
};

} // namespace Core
} // namespace Acheron
