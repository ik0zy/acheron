#pragma once

#include <QSize>
#include <QtGlobal>

#include <array>
#include <optional>

#include "Core/Video/SendRateController.hpp"

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

// The lower of the estimates there are of what the way to the server carries, in bits per
// second: the receiving side's own, and the one made from when each packet reached it. Zero
// when there is neither. The second is left out when the setting `video/delay_estimate` is
// false.
[[nodiscard]] qint64 estimatedCeiling(quint64 receiverEstimate, quint64 delayEstimate);

// The account's own camera. It is captured at one size and rate, and what is encoded of it
// follows how much of it the viewers want, which the server relays as a number from 0 to 100.
inline constexpr QSize CAMERA_CAPTURE_SIZE(1280, 720);
inline constexpr int CAMERA_FRAMERATE = 30;
inline constexpr int CAMERA_MIN_BITRATE = 150'000;
inline constexpr int CAMERA_MAX_BITRATE = 2'500'000;

// The official client sends its camera in two encodings, each on SSRCs of its own, and a
// viewer takes one or the other: the second where the picture is shown small, and wherever
// it is the only camera in the call. This client does so when the setting
// `video/camera_second_layer` is true. It is false until it has been tried against Discord.
[[nodiscard]] bool cameraSecondLayerEnabled();
inline constexpr int CAMERA_FULL_QUALITY = 100;
inline constexpr int CAMERA_SECOND_QUALITY = 50;

// One row of the camera's ladder.
struct CameraLayer
{
    // The least want the row is used for.
    int want = 0;
    QSize size;
    // Bits per second.
    int maxBitrate = 0;

    bool operator==(const CameraLayer &other) const = default;
};

// The most a camera picture of this many pixels is sent at: the full picture's bitrate by its
// share of the area, and never under the minimum.
[[nodiscard]] int cameraMaxBitrate(qint64 pixels);
// The row with the highest want not above `want`, and the lowest row for less than any.
[[nodiscard]] CameraLayer cameraLayerFor(int want);
// A camera whose owner is not heard is sent at fewer frames a second, and at fewer still when
// no more than the lowest row is wanted of it.
[[nodiscard]] int cameraFramerate(int want, bool muted);

// What the camera's encoder is asked for.
struct CameraEncoding
{
    QSize size;
    int framerate = 0;
    // Bits per second.
    int bitrate = 0;

    bool operator==(const CameraEncoding &other) const = default;
};

// Decides what the camera is encoded at from what the viewers want, whether its owner is heard,
// and what the server says is lost on the way. It has no clock: the owner passes the time in.
//
// The owner counts as muted while self-muted, or once nothing has been said for
// SILENCE_BEFORE_MUTED_MS, unless a screen is being shared as well. The frame rate follows
// that at once. A new size or bitrate is taken up only SETTLE_MS after the last one, because
// each costs the encoder a key frame and wants arrive in quick succession.
class CameraSendPlan
{
public:
    // The camera is turned on: all of it is wanted until the server says otherwise, it is sent
    // at the most the full picture may be, and the silence is counted from now.
    void start(qint64 nowMs);

    void setWant(int wanted);
    void setSelfMuted(bool muted);
    void setSpeaking(bool nowSpeaking, qint64 nowMs);
    void setSharingScreen(bool sharing);
    // One of the server's reports, as SendRateController::report takes it.
    void reportLoss(double lossFraction, qint64 nowMs);
    // The receiving side's estimate, as SendRateController::setReceiverLimit takes it.
    void setReceiverLimit(qint64 bitsPerSecond);

    // Takes up what has become due by `nowMs` and returns what the encoder is to be doing.
    CameraEncoding advance(qint64 nowMs);
    // How long after `nowMs` advance() would first answer differently with nothing new having
    // been said. Empty when it would not.
    [[nodiscard]] std::optional<qint64> nextChangeIn(qint64 nowMs) const;

    // The want the size and frame rate in use were chosen for.
    [[nodiscard]] int servedWant() const { return wantInUse; }
    [[nodiscard]] int maxBitrate() const { return cameraLayerFor(wantInUse).maxBitrate; }
    [[nodiscard]] bool countsAsMuted(qint64 nowMs) const;

    static constexpr qint64 SETTLE_MS = 2000;
    static constexpr qint64 SILENCE_BEFORE_MUTED_MS = 15000;

private:
    [[nodiscard]] bool changeIsWaiting() const;

    int want = 100;
    int wantInUse = 100;
    bool selfMuted = false;
    bool speaking = false;
    bool sharingScreen = false;
    qint64 silentSinceMs = 0;
    SendRateController rate;
    int bitrateInUse = 0;
    qint64 changedAtMs = 0;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
