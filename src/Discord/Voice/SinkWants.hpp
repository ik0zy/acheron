#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>

#include "Core/Snowflake.hpp"
#include "VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// One of the encodings a user sends their video in. A camera usually comes as two: quality 100
// and a smaller quality 50.
struct VideoLayer
{
    quint32 ssrc = 0;
    quint32 rtxSsrc = 0;
    int quality = 100;

    bool operator==(const VideoLayer &other) const = default;
};

// Someone in the call who is sending video, and what this client wants of it.
struct VideoSender
{
    Core::Snowflake userId;
    QList<VideoLayer> layers;
    // False when their video is not to be received at all: no tile for it, disabled by the
    // user, or out of view for too long.
    bool wanted = true;
    // The size their tile is drawn at, or 0 when that is not known.
    int tilePixels = 0;
};

struct SinkWantsInput
{
    QList<VideoSender> senders;
    // The participant shown large, if any.
    Core::Snowflake focusedUser;
    // A call between two people, where the official client also sizes its request to the tile.
    bool oneToOne = false;
};

struct SinkWantsPlan
{
    // What to tell the server (opcode 15).
    MediaSinkWants wants;
    // For each wanted sender, the SSRC their video is to be taken from.
    QHash<Core::Snowflake, quint32> deliver;
};

// How much video to ask of each sender when there are `videoSenders` of them: the official
// client's ladder, which shares 1280x720 worth of pixels between everyone, and steps down
// further for a tile smaller than the step.
[[nodiscard]] int sinkWantLevel(int videoSenders, int tilePixels = 0);

// The official desktop client's rules for a voice call's opcode 15.
[[nodiscard]] SinkWantsPlan planSinkWants(const SinkWantsInput &input);

} // namespace Voice
} // namespace Discord
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Discord::Voice::VideoLayer)
