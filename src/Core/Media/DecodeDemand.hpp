#pragma once

#include <QtGlobal>

#include <optional>

namespace Acheron {
namespace Core {
namespace Media {

// How much of a picture to ask its sender for, going by how the decoder keeps up with it.
//
// A decoder with time to spare works a backlog off by itself within moments, so a short one
// means nothing. One that stays behind is too slow for what is being sent, and the only cure
// is being sent less: the share of the picture asked for is halved, and raised again once the
// decoder has kept up for a while, after longer each time a raise has had to be taken back.
//
// It keeps no clock: the owner reports the decoder's state about once a second.
class DecodeDemand
{
public:
    // Returns whether the share asked for changed.
    bool observe(int backlogMs, quint64 queuesDiscarded, qint64 nowMs);

    [[nodiscard]] double pixelShare() const { return 1.0 / (1 << halvings); }
    // The pixel count to ask for, of a picture shown at `shownPixels` and sent at `sentPixels`
    // (zero while that is not known).
    [[nodiscard]] int pixelsToAskFor(int shownPixels, qint64 sentPixels) const;

    static constexpr int BEHIND_MS = 250;
    static constexpr int BEHIND_FOR_MS = 2000;
    static constexpr int SETTLE_MS = 5000;
    static constexpr int MAX_HALVINGS = 2;
    static constexpr int FIRST_RAISE_AFTER_MS = 60'000;
    static constexpr int LONGEST_RAISE_AFTER_MS = 480'000;
    // A raise taken back within this long was a mistake, and the next one waits twice as long.
    static constexpr int RAISE_ON_TRIAL_MS = 60'000;

private:
    void lower(qint64 nowMs);

    int halvings = 0;
    quint64 discardsSeen = 0;
    std::optional<qint64> behindSinceMs;
    std::optional<qint64> keepingUpSinceMs;
    std::optional<qint64> loweredAtMs;
    std::optional<qint64> raisedAtMs;
    int raiseAfterMs = FIRST_RAISE_AFTER_MS;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
