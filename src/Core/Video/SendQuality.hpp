#pragma once

#include <QSize>
#include <QtGlobal>

#include <array>

namespace Acheron {
namespace Core {
namespace Video {

// What a screen share is sent at, as its owner chose it. The rules here are the official
// client's (docs/video-re/re-voice-gateway-video.md).
struct SendQuality
{
    static constexpr int SOURCE_HEIGHT = 0;

    // Lines of the picture. SOURCE_HEIGHT sends the screen at its own size.
    int height = 720;
    int framerate = 30;

    [[nodiscard]] bool isSource() const { return height == SOURCE_HEIGHT; }
    bool operator==(const SendQuality &other) const = default;
};

inline constexpr std::array<int, 5> SEND_HEIGHTS{ 480, 720, 1080, 1440, SendQuality::SOURCE_HEIGHT };
inline constexpr std::array<int, 3> SEND_FRAMERATES{ 15, 30, 60 };

// What unlocks the higher qualities: the account's Nitro, and the boosts of the guild it shares in.
struct SharerTier
{
    // The user's `premium_type`: 1 is Nitro Classic, 2 Nitro, 3 Nitro Basic.
    int premiumType = 0;
    // The guild's `premium_tier`, 0 to 3. A private call has none.
    int guildBoostTier = 0;
};

[[nodiscard]] bool mayUseQuality(const SendQuality &quality, const SharerTier &tier);
// `wanted` when the account may use it, and otherwise 720p at 30, which everyone may.
[[nodiscard]] SendQuality usableQuality(const SendQuality &wanted, const SharerTier &tier);
// `quality` moved to another height, at 30 frames a second where its own rate is not allowed
// there.
[[nodiscard]] SendQuality atHeight(const SendQuality &quality, int height, const SharerTier &tier);

// The largest picture announced for a quality: 4:3 at 480 lines and 16:9 above. The source's
// own size is announced as zero by zero.
[[nodiscard]] QSize announcedSize(const SendQuality &quality);

// Bits per second.
struct BitrateLimits
{
    int min = 0;
    int target = 0;
    int max = 0;

    bool operator==(const BitrateLimits &other) const = default;
};

[[nodiscard]] BitrateLimits bitrateLimits(const SendQuality &quality);

// The limits for a picture of `encodedPixels` that its largest viewer shows at `viewerPixels`:
// a viewer with a small tile is not sent the bits of a full one. The picture's size stays as
// it is. Zero `viewerPixels` means the server has not said. For a very small picture the
// maximum can come out under the minimum, as it does in the official client.
[[nodiscard]] BitrateLimits limitsForViewerSize(const BitrateLimits &full, const SendQuality &quality, qint64 encodedPixels, int framerate, qint64 viewerPixels);

// The viewers' size as the bitrate follows it: a new size is taken up only when it is more
// than a twentieth away from the one in use, so that a window being dragged does not move the
// encoder with it.
class ViewerSize
{
public:
    // Whether the size was taken up.
    bool offer(qint64 pixelCount);
    [[nodiscard]] qint64 pixelCount() const { return adopted; }

private:
    qint64 adopted = 0;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
