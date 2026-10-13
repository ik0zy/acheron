#pragma once

#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <optional>

#include "Rtcp.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// What the way from this client to the server carries, judged from the server's reports of
// when each video packet reached it (transport-wide congestion control). Packets that arrive
// further and further apart than they left are queueing somewhere, which shows before any is
// lost. It numbers the connection's video packets, which is what those reports go by.
//
// This is the delay-based half of WebRTC's GoogCC, which the official client runs unchanged:
// inter_arrival_delta.cc, trendline_estimator.cc, bitrate_estimator.cc, aimd_rate_control.cc
// and link_capacity_estimator.cc, with their numbers. Two things are left out. Nothing is
// sent to probe the way, so it knows of no limit until the way has once been overused, and
// then finds its way back up only as fast as what is sent grows.
//
// It has no clock: the owner passes the time in, in microseconds from any origin.
class SendSideEstimator
{
public:
    // Forgets everything, and numbers the next packet `firstSequence`.
    void restart(uint16_t firstSequence);

    // Numbers a packet that is about to leave.
    uint16_t numberPacket(qint64 nowUs, int bytes);
    // The round trip the server's reception reports gave.
    void setRoundTrip(qint64 roundTripUs);
    // One of the server's reports. Returns whether the estimate changed.
    bool onReport(const ReceivedRtcp::TransportReport &report, qint64 nowUs);

    // Bits per second. Zero until the way has once been overused.
    [[nodiscard]] qint64 estimate() const { return rate.initialized ? static_cast<qint64>(rate.current) : 0; }
    // What the server has lately acknowledged receiving, in bits per second. Zero until that
    // has been measured.
    [[nodiscard]] qint64 acknowledged() const { return throughput.estimateKbps < 0 ? 0 : static_cast<qint64>(throughput.estimateKbps * 1000); }
    [[nodiscard]] quint64 reportsTaken() const { return reports; }
    [[nodiscard]] bool overusing() const { return trend.usage == Usage::Overusing; }

    static constexpr int MAX_PACKETS_HELD = 20000;
    static constexpr qint64 HELD_FOR_US = 60'000'000;
    static constexpr qint64 SILENCE_BEFORE_RESTART_US = 2'000'000;

private:
    enum class Usage {
        Normal,
        Underusing,
        Overusing,
    };

    struct Sent
    {
        qint64 sentUs = 0;
        // Zero once the server has reported on it.
        int bytes = 0;
    };

    // What is acknowledged per second, measured over windows and smoothed by how far each
    // window is from the estimate.
    struct Throughput
    {
        void add(qint64 atMs, int bytes);
        [[nodiscard]] float closeWindow(qint64 atMs, int bytes, int windowMs);

        float estimateKbps = -1;
        float variance = 50;
        qint64 sum = 0;
        qint64 windowMs = 0;
        qint64 previousMs = -1;
    };

    // Packets sent within a few milliseconds of each other are taken as one group, and each
    // group's spacing from the one before is compared with the spacing they arrived at.
    struct Grouping
    {
        struct Group
        {
            qint64 firstSendUs = -1;
            qint64 sendUs = -1;
            qint64 firstArrivalUs = -1;
            qint64 completeUs = -1;
            qint64 lastSystemUs = -1;
            [[nodiscard]] bool isEmpty() const { return completeUs < 0; }
        };

        // True when a group was completed, with the spacings in the two outputs.
        bool add(qint64 sendUs, qint64 arrivalUs, qint64 systemUs, qint64 *sendDeltaUs, qint64 *arrivalDeltaUs);
        [[nodiscard]] bool belongsToBurst(qint64 arrivalUs, qint64 sendUs) const;

        Group current;
        Group previous;
        int reorderedInARow = 0;
    };

    // The slope of the delay over the last groups, held against a threshold that follows it.
    struct Trend
    {
        struct Point
        {
            double arrivalMs = 0;
            double smoothedDelayMs = 0;
        };

        void add(double arrivalDeltaMs, double sendDeltaMs, double arrivalMs);
        void detect(double slope, double sendDeltaMs, double nowMs);
        void adaptThreshold(double modifiedTrend, double nowMs);

        std::deque<Point> window;
        double firstArrivalMs = -1;
        double accumulatedDelayMs = 0;
        double smoothedDelayMs = 0;
        int deltas = 0;
        double threshold = 12.5;
        double thresholdAdaptedMs = -1;
        double previousSlope = 0;
        double overusingForMs = -1;
        int overuses = 0;
        Usage usage = Usage::Normal;
    };

    // Where the way has been seen to give out, and how sure that is.
    struct Capacity
    {
        void onOveruse(double bitsPerSecond);
        [[nodiscard]] double upperBound() const;
        [[nodiscard]] double lowerBound() const;
        [[nodiscard]] double deviationKbps() const;

        std::optional<double> estimateKbps;
        double deviation = 0.4;
    };

    // Additive increase, multiplicative decrease.
    struct Rate
    {
        enum class State {
            Hold,
            Increase,
            Decrease,
        };

        [[nodiscard]] bool timeToReduceFurther(qint64 nowUs, double acknowledged) const;
        // Returns whether the bitrate changed.
        bool update(Usage usage, std::optional<double> acknowledged, qint64 nowUs);
        [[nodiscard]] double nearMaxIncreasePerSecond() const;

        State state = State::Hold;
        double current = 30'000'000;
        double latestAcknowledged = 30'000'000;
        bool initialized = false;
        qint64 changedUs = -1;
        qint64 roundTripUs = 200'000;
        Capacity capacity;
    };

    void forgetDelays();

    std::deque<Sent> sent;
    // The number of the first packet in `sent`, and of the next to leave, neither wrapped.
    qint64 firstHeld = 0;
    qint64 nextNumber = 0;

    bool haveReference = false;
    int32_t lastReference = 0;
    qint64 referenceOffsetUs = 0;
    qint64 lastArrivalSeenUs = -1;
    quint64 reports = 0;

    Throughput throughput;
    Grouping grouping;
    Trend trend;
    Rate rate;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
