#include "Core/Video/SendRateController.hpp"

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace Core {
namespace Video {

void SendRateController::restart(int minBitrate, int maxBitrate)
{
    ceiling = maxBitrate;
    floor = std::min(minBitrate, maxBitrate);
    current = ceiling;
    receiverLimit.reset();
    loweredAtMs.reset();
    cleanSinceMs.reset();
}

int SendRateController::bitrate() const
{
    if (!receiverLimit)
        return current;
    return static_cast<int>(std::max<qint64>(floor, std::min<qint64>(current, *receiverLimit)));
}

bool SendRateController::setReceiverLimit(qint64 bitsPerSecond)
{
    const int before = bitrate();
    if (bitsPerSecond <= 0)
        receiverLimit.reset();
    else if (!receiverLimit || bitsPerSecond < *receiverLimit || bitsPerSecond >= *receiverLimit * INCREASE)
        receiverLimit = bitsPerSecond;
    return bitrate() != before;
}

bool SendRateController::setLimits(int minBitrate, int maxBitrate)
{
    const int before = bitrate();
    const double share = ceiling > 0 ? static_cast<double>(current) / ceiling : 1.0;
    ceiling = maxBitrate;
    floor = std::min(minBitrate, maxBitrate);
    moveTo(share * ceiling);
    return bitrate() != before;
}

bool SendRateController::report(double lossFraction, qint64 nowMs)
{
    if (lossFraction < 0)
        return false;

    if (lossFraction >= LOW_LOSS)
        cleanSinceMs.reset();

    if (lossFraction > HIGH_LOSS) {
        if (loweredAtMs && nowMs - *loweredAtMs < DECREASE_INTERVAL_MS)
            return false;
        loweredAtMs = nowMs;
        // From what is in use, which the receiver's limit may hold well under what losses
        // had allowed.
        const int before = bitrate();
        moveTo(before * DECREASE);
        return bitrate() != before;
    }

    if (lossFraction >= LOW_LOSS)
        return false;

    if (!cleanSinceMs)
        cleanSinceMs = nowMs;
    if (nowMs - *cleanSinceMs < CLEAN_BEFORE_INCREASE_MS)
        return false;
    cleanSinceMs = nowMs;
    const int before = bitrate();
    moveTo(current * INCREASE);
    return bitrate() != before;
}

void SendRateController::moveTo(double wanted)
{
    current = std::clamp(static_cast<int>(std::lround(wanted)), floor, ceiling);
}

} // namespace Video
} // namespace Core
} // namespace Acheron
