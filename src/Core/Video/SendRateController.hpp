#pragma once

#include <QtGlobal>

#include <optional>

namespace Acheron {
namespace Core {
namespace Video {

// Chooses the bitrate a video is encoded at, between its limits, from what the server says is
// being lost on the way. It starts at the most allowed, drops by a third when a tenth of the
// packets go missing, and climbs back slowly once losses stay under a fiftieth.
//
// It has no clock: the owner passes the time in with each of the server's reports. Every
// change it makes can cost the encoder a key frame, so it makes few.
class SendRateController
{
public:
    // Starts over at the maximum.
    void restart(int minBitrate, int maxBitrate);
    // New limits for the same video. The bitrate keeps its share of the maximum, so one that
    // had been lowered stays lowered. Returns whether the bitrate changed.
    bool setLimits(int minBitrate, int maxBitrate);
    // One report of the share of packets lost, 0 to 1; a negative share means nothing has
    // been reported yet and is ignored. Returns whether the bitrate changed.
    bool report(double lossFraction, qint64 nowMs);

    [[nodiscard]] int bitrate() const { return current; }

    static constexpr double HIGH_LOSS = 0.10;
    static constexpr double LOW_LOSS = 0.02;
    static constexpr double DECREASE = 0.7;
    static constexpr double INCREASE = 1.15;
    static constexpr qint64 DECREASE_INTERVAL_MS = 2000;
    static constexpr qint64 CLEAN_BEFORE_INCREASE_MS = 10000;

private:
    bool moveTo(double bitrate);

    int floor = 0;
    int ceiling = 0;
    int current = 0;
    std::optional<qint64> loweredAtMs;
    std::optional<qint64> cleanSinceMs;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
