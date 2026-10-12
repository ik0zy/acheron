#include "Core/Video/SendQuality.hpp"

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace Core {
namespace Video {

namespace {

constexpr int NITRO_CLASSIC = 1;
constexpr int NITRO = 2;

constexpr int MIN_BITRATE = 500'000;
constexpr int TARGET_BITRATE = 600'000;
constexpr int STANDARD_MAX_BITRATE = 3'500'000;
constexpr int HIGH_MAX_BITRATE = 9'000'000;
constexpr int BITRATE_FLOOR = 150'000;

constexpr int STANDARD_HEIGHT = 720;
constexpr int STANDARD_FRAMERATE = 30;
constexpr int SLOW_SOURCE_FRAMERATE = 10;

// A scaled-down maximum still leaves a fiftieth of a bit for each pixel of each frame.
constexpr int PIXEL_FRAMES_PER_BIT = 50;

template <typename Values>
bool contains(const Values &values, int value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

int scaledUp(double share, int bitrate)
{
    return static_cast<int>(std::ceil(share * bitrate));
}

} // namespace

bool mayUseQuality(const SendQuality &quality, const SharerTier &tier)
{
    if (!contains(SEND_HEIGHTS, quality.height) || !contains(SEND_FRAMERATES, quality.framerate))
        return false;

    if (quality.isSource())
        return tier.premiumType == NITRO;

    const bool paysForIt = tier.premiumType == NITRO_CLASSIC || tier.premiumType == NITRO;
    if (quality.height > STANDARD_HEIGHT)
        return paysForIt || tier.guildBoostTier >= 2;
    if (quality.framerate > STANDARD_FRAMERATE)
        return paysForIt || tier.guildBoostTier >= 1;
    return true;
}

SendQuality usableQuality(const SendQuality &wanted, const SharerTier &tier)
{
    return mayUseQuality(wanted, tier) ? wanted : SendQuality{};
}

SendQuality atHeight(const SendQuality &quality, int height, const SharerTier &tier)
{
    SendQuality moved{ height, quality.framerate };
    if (!mayUseQuality(moved, tier))
        moved.framerate = STANDARD_FRAMERATE;
    return moved;
}

QSize announcedSize(const SendQuality &quality)
{
    if (quality.isSource())
        return QSize(0, 0);
    const int width = quality.height == 480 ? quality.height * 4 / 3 : quality.height * 16 / 9;
    return QSize(width, quality.height);
}

BitrateLimits bitrateLimits(const SendQuality &quality)
{
    const bool standard = quality.isSource() ? quality.framerate < SLOW_SOURCE_FRAMERATE : quality.height <= STANDARD_HEIGHT && quality.framerate <= STANDARD_FRAMERATE;
    return { MIN_BITRATE, TARGET_BITRATE, standard ? STANDARD_MAX_BITRATE : HIGH_MAX_BITRATE };
}

BitrateLimits limitsForViewerSize(const BitrateLimits &full, const SendQuality &quality, qint64 encodedPixels, int framerate, qint64 viewerPixels)
{
    if (quality.isSource() || viewerPixels <= 0 || viewerPixels >= encodedPixels)
        return full;

    const double share = static_cast<double>(viewerPixels) / static_cast<double>(encodedPixels);
    const qint64 leastMax = std::min<qint64>(encodedPixels * framerate / PIXEL_FRAMES_PER_BIT, full.max);

    BitrateLimits scaled;
    scaled.min = std::max(scaledUp(share, full.min), BITRATE_FLOOR);
    scaled.target = std::max(scaledUp(share, full.target), BITRATE_FLOOR);
    scaled.max = std::max(scaledUp(share, full.max), static_cast<int>(leastMax));
    return scaled;
}

bool ViewerSize::offer(qint64 pixelCount)
{
    const qint64 difference = pixelCount > adopted ? pixelCount - adopted : adopted - pixelCount;
    if (difference * 20 <= adopted)
        return false;
    adopted = pixelCount;
    return true;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
