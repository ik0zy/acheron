#pragma once

#include <QString>
#include <QUrl>

#include <optional>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Discord {

// Names one Go Live stream: the channel it is in and who owns it. The gateway addresses
// streams by the string form, `guild:<guild>:<channel>:<owner>` or `call:<channel>:<owner>`.
struct StreamKey
{
    enum class Type {
        Guild,
        Call,
    };

    Type type = Type::Guild;
    Core::Snowflake guildId;
    Core::Snowflake channelId;
    Core::Snowflake ownerId;

    [[nodiscard]] static StreamKey forChannel(Core::Snowflake guildId, Core::Snowflake channelId, Core::Snowflake ownerId);
    [[nodiscard]] static std::optional<StreamKey> parse(const QString &key);

    [[nodiscard]] QString toString() const;
    // The `type` the gateway's STREAM_CREATE takes for this kind of stream.
    [[nodiscard]] QString typeName() const;

    bool operator==(const StreamKey &other) const = default;
};

// The answer to asking for the thumbnail a stream's owner last uploaded. On failure `url` is
// empty; a 429 also carries how long the server asks the client to wait.
struct StreamPreviewLookup
{
    QUrl url;
    int statusCode = 0;
    qint64 retryAfterMs = 0;
};

} // namespace Discord
} // namespace Acheron
