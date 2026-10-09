#pragma once

#include <QHash>
#include <QtGlobal>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {

// Decides whose video is still worth receiving once their tile has gone out of view, the way
// the official client does: a tile that leaves the screen keeps its video for a while in case
// it comes straight back, but only the few most recently hidden ones do, and none for long.
//
// It has no clock of its own; the caller passes the time in.
class TileVisibility
{
public:
    void setVisible(Snowflake userId, bool visible, qint64 nowMs);
    void remove(Snowflake userId);
    void clear();

    // Whether the user's video should be received. False for someone with no tile at all.
    [[nodiscard]] bool wanted(Snowflake userId, qint64 nowMs) const;

    // Whether a tile is hidden but still inside its grace period, so the answer will change
    // by itself as time passes.
    [[nodiscard]] bool hasPendingExpiry(qint64 nowMs) const;

    static constexpr int HIDDEN_GRACE_MS = 30000;
    static constexpr int HIDDEN_KEPT = 3;

private:
    struct Tile
    {
        bool visible = false;
        qint64 hiddenSinceMs = 0;
    };

    QHash<Snowflake, Tile> tiles;
};

} // namespace Core
} // namespace Acheron
