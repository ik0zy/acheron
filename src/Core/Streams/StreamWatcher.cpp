#include "StreamWatcher.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace Core {

StreamWatcher::StreamWatcher(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, Snowflake accountId, const ProxyConfig &proxy, QObject *parent)
    : QObject(parent), client(client), streams(streams), voice(voice), accountId(accountId), proxy(proxy)
{
    connect(streams, &StreamManager::streamChanged, this, &StreamWatcher::onStreamChanged);
    connect(streams, &StreamManager::streamEnded, this, &StreamWatcher::onStreamEnded);
    connect(voice, &Audio::VoiceManager::voiceStateChanged, this, &StreamWatcher::onVoiceStateChanged);
    connect(voice, &Audio::VoiceManager::voiceDisconnected, this, &StreamWatcher::stopAll);
    connect(voice, &Audio::VoiceManager::voiceConnected, this, &StreamWatcher::autoWatch);
    connect(voice, &Audio::VoiceManager::participantJoined, this, &StreamWatcher::autoWatch);
    connect(voice, &Audio::VoiceManager::participantUpdated, this, &StreamWatcher::autoWatch);
}

void StreamWatcher::watchWhenConnected(const Discord::StreamKey &key)
{
    if (canWatch(key)) {
        watch(key);
        return;
    }
    pendingWatch = key;
}

// In a private call the official client watches a stream without being asked, when the call
// is joined and when someone in it goes live.
void StreamWatcher::autoWatch()
{
    if (!voice->isConnected() || voice->currentGuildId().isValid())
        return;

    const Snowflake channelId = voice->currentChannelId();
    const QList<Audio::VoiceParticipant> participants = voice->currentParticipants();
    for (const Audio::VoiceParticipant &participant : participants) {
        if (participant.userId == accountId)
            continue;
        const Discord::StreamKey key = Discord::StreamKey::forChannel(Snowflake::Invalid, channelId, participant.userId);
        const QString streamKey = key.toString();
        if (!participant.selfStream) {
            fullStreams.remove(streamKey);
            continue;
        }
        if (!sessions.contains(streamKey) && !fullStreams.contains(streamKey))
            watch(key);
    }
}

void StreamWatcher::setGatewaySessionId(const QString &sessionId)
{
    gatewaySessionId = sessionId;
}

bool StreamWatcher::canWatch(const Discord::StreamKey &key) const
{
    return key.ownerId != accountId && voice->isConnected() && voice->currentChannelId() == key.channelId;
}

StreamSession *StreamWatcher::session(const QString &streamKey) const
{
    return sessions.value(streamKey, nullptr);
}

void StreamWatcher::watch(const Discord::StreamKey &key)
{
    if (!canWatch(key))
        return;

    const QString streamKey = key.toString();
    if (StreamSession *existing = sessions.value(streamKey, nullptr)) {
        if (!existing->isOver()) {
            emit sessionOpened(existing);
            return;
        }
        remove(streamKey, false);
    }

    // Watching replaces what was being watched, unless two or more are already open.
    if (sessions.size() < 2) {
        const QStringList others = sessions.keys();
        for (const QString &other : others)
            remove(other, true);
    }

    auto *session = new StreamSession(key, accountId, gatewaySessionId, proxy, voice, nextAudioSource++, this);
    connect(session, &StreamSession::pingRequested, this, [this, streamKey] { client->sendStreamPing(streamKey); });
    sessions.insert(streamKey, session);

    client->sendStreamWatch(streamKey);
    emit sessionOpened(session);
}

void StreamWatcher::stopWatching(const QString &streamKey)
{
    remove(streamKey, true);
}

void StreamWatcher::stopAll()
{
    const QStringList keys = sessions.keys();
    for (const QString &key : keys)
        remove(key, true);
}

void StreamWatcher::reset()
{
    pendingWatch.reset();
    fullStreams.clear();
    const QStringList keys = sessions.keys();
    for (const QString &key : keys)
        remove(key, false);
}

void StreamWatcher::remove(const QString &streamKey, bool tellGateway)
{
    StreamSession *session = sessions.take(streamKey);
    if (!session)
        return;

    if (tellGateway && !session->isOver())
        client->sendStreamDelete(streamKey);

    emit sessionClosed(streamKey);
    delete session;
}

void StreamWatcher::onStreamChanged(const QString &streamKey)
{
    StreamSession *session = sessions.value(streamKey, nullptr);
    const std::optional<StreamRecord> record = streams->stream(streamKey);
    if (session && record)
        session->apply(*record);
}

void StreamWatcher::onStreamEnded(const QString &streamKey, StreamEndReason reason)
{
    if (reason == StreamEndReason::StreamFull)
        fullStreams.insert(streamKey);
    if (StreamSession *session = sessions.value(streamKey, nullptr))
        session->end(reason);
}

void StreamWatcher::onVoiceStateChanged()
{
    // Leaving the channel a stream is in stops watching it.
    const QStringList keys = sessions.keys();
    for (const QString &key : keys) {
        StreamSession *session = sessions.value(key, nullptr);
        if (session && voice->currentChannelId() != session->key().channelId)
            remove(key, true);
    }

    if (pendingWatch && canWatch(*pendingWatch)) {
        const Discord::StreamKey key = *pendingWatch;
        pendingWatch.reset();
        watch(key);
    } else if (pendingWatch && voice->isConnected() && voice->currentChannelId() != pendingWatch->channelId) {
        pendingWatch.reset();
    }
}

} // namespace Core
} // namespace Acheron
