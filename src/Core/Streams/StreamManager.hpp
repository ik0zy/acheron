#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <optional>

#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"
#include "Discord/StreamKey.hpp"

namespace Acheron {
namespace Core {

// What the gateway has said about one stream this account owns or asked to watch.
struct StreamRecord
{
    enum class State {
        // STREAM_CREATE arrived; the server has not yet said where the stream is hosted.
        AwaitingEndpoint,
        // An endpoint and token are known, so an RTC connection can be opened.
        Ready,
        // The server marked the stream unavailable and is expected to announce it again.
        Reconnecting,
    };

    Discord::StreamKey key;
    State state = State::AwaitingEndpoint;
    QString region;
    QList<Snowflake> viewerIds;
    bool paused = false;

    Snowflake rtcServerId;
    Snowflake rtcChannelId;
    QString endpoint;
    QString token;
};

enum class StreamEndReason {
    UserRequested,
    StreamFull,
    Unauthorized,
    SafetyGuildRateLimited,
    Other,
};

[[nodiscard]] StreamEndReason streamEndReasonFromString(const QString &reason);

// Tracks the streams the gateway is telling this account about. The gateway only sends stream
// events to a stream's owner and its viewers, so this is empty until the account starts or
// watches one; who else is live comes from voice states.
class StreamManager : public QObject
{
    Q_OBJECT
public:
    explicit StreamManager(QObject *parent = nullptr);

    void handleStreamCreate(const Discord::StreamCreate &event);
    void handleStreamServerUpdate(const Discord::StreamServerUpdate &event);
    void handleStreamUpdate(const Discord::StreamUpdate &event);
    void handleStreamDelete(const Discord::StreamDelete &event);

    // A new gateway session forgets every stream; a resumed one keeps them.
    void reset();

    [[nodiscard]] std::optional<StreamRecord> stream(const QString &streamKey) const;
    [[nodiscard]] QStringList streamKeys() const;

signals:
    void streamChanged(const QString &streamKey);
    void streamEnded(const QString &streamKey, Acheron::Core::StreamEndReason reason);

private:
    QHash<QString, StreamRecord> streams;
};

} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::StreamEndReason)
