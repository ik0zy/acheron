#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <optional>

#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"

namespace Acheron {
namespace Core {

// What the gateway has said about the call in one DM or group DM.
struct CallRecord
{
    Snowflake channelId;
    Snowflake messageId;
    // Unset both before the gateway has answered and after the call has ended.
    std::optional<QString> region;
    // Who is being rung, mapped to who rang them.
    QHash<Snowflake, Snowflake> ongoingRings;
    bool unavailable = false;

    [[nodiscard]] bool isActive() const { return !unavailable && region.has_value(); }
};

// The private calls this account's gateway session knows about, kept the way the official
// client's CallStore keeps them. Calls are never part of READY: they arrive as CALL_CREATE, on
// connect and in answer to CALL_CONNECT (opcode 13).
class CallStore : public QObject
{
    Q_OBJECT
public:
    explicit CallStore(QObject *parent = nullptr);

    // Whether to ask the gateway about a private channel's call now that the channel is being
    // looked at. True once per channel, or every time when forced (a fresh or resumed session);
    // the channel is remembered either way so that looking at it again does not ask again.
    [[nodiscard]] bool shouldConnect(Snowflake channelId, bool force);

    void handleCallCreate(const Discord::CallCreate &event);
    void handleCallUpdate(const Discord::CallUpdate &event);
    void handleCallDelete(const Discord::CallDelete &event);
    void forget(Snowflake channelId);
    void reset();

    [[nodiscard]] std::optional<CallRecord> call(Snowflake channelId) const;
    [[nodiscard]] bool isActive(Snowflake channelId) const;
    [[nodiscard]] bool isRinging(Snowflake channelId, Snowflake userId) const;

signals:
    void callChanged(Acheron::Core::Snowflake channelId);

private:
    QHash<Snowflake, CallRecord> calls;
};

} // namespace Core
} // namespace Acheron
