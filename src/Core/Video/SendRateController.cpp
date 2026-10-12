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
    loweredAtMs.reset();
    cleanSinceMs.reset();
}

bool SendRateController::setLimits(int minBitrate, int maxBitrate)
{
    const double share = ceiling > 0 ? static_cast<double>(current) / ceiling : 1.0;
    ceiling = maxBitrate;
    floor = std::min(minBitrate, maxBitrate);
    return moveTo(share * ceiling);
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
        return moveTo(current * DECREASE);
    }

    if (lossFraction >= LOW_LOSS)
        return false;

    if (!cleanSinceMs)
        cleanSinceMs = nowMs;
    if (nowMs - *cleanSinceMs < CLEAN_BEFORE_INCREASE_MS)
        return false;
    cleanSinceMs = nowMs;
    return moveTo(current * INCREASE);
}

bool SendRateController::moveTo(double bitrate)
{
    const int bounded = std::clamp(static_cast<int>(std::lround(bitrate)), floor, ceiling);
    if (bounded == current)
        return false;
    current = bounded;
    return true;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
