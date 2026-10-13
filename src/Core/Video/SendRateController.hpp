#pragma once

#include <QtGlobal>

#include <optional>

namespace Acheron {
namespace Core {
namespace Video {

// Chooses the bitrate a video is encoded at, between its limits, from what the server says is
// being lost on the way. It starts at the most allowed, drops by a third when a tenth of the
// packets go missing, and climbs back slowly once losses stay under a fiftieth. What the
// receiving side says the way carries is a ceiling on all of that, as it is to the official
// client's congestion control.
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
    // The receiving side's estimate of what the way carries (REMB), in bits per second; zero
    // lifts it. A lower one is followed at once. A higher one only once it is a whole step up,
    // since the estimate moves all the time and every change here can cost a key frame.
    // Returns whether the bitrate changed.
    bool setReceiverLimit(qint64 bitsPerSecond);

    [[nodiscard]] int bitrate() const;

    static constexpr double HIGH_LOSS = 0.10;
    static constexpr double LOW_LOSS = 0.02;
    static constexpr double DECREASE = 0.7;
    static constexpr double INCREASE = 1.15;
    static constexpr qint64 DECREASE_INTERVAL_MS = 2000;
    static constexpr qint64 CLEAN_BEFORE_INCREASE_MS = 10000;

private:
    void moveTo(double wanted);

    int floor = 0;
    int ceiling = 0;
    // What losses alone allow. The receiver's limit is applied on top, so that it leaves no
    // mark once it is lifted.
    int current = 0;
    std::optional<qint64> receiverLimit;
    std::optional<qint64> loweredAtMs;
    std::optional<qint64> cleanSinceMs;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
