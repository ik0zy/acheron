#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Acheron {
namespace Core {
namespace Media {
namespace H264 {

// The official client rewrites the SPS of the H264 it receives before it decrypts the frame, so
// a frame whose SPS it rewrites is no longer the one its sender encrypted, and is never shown.
// It rewrites every SPS that does not say, in the bitstream restriction of its VUI, that
// pictures come out in the order they were coded and that no more of them are held than are
// referred to. What is here makes the same change first, so that what is sent is left alone.

// What an SPS says of the order its pictures come out in.
struct SpsReordering
{
    bool hasVui = false;
    bool hasBitstreamRestriction = false;
    uint32_t maxNumReorderFrames = 0;
    uint32_t maxDecFrameBuffering = 0;
    uint32_t maxNumRefFrames = 0;

    [[nodiscard]] bool isKeptByOfficialClient() const { return hasBitstreamRestriction && maxNumReorderFrames == 0 && maxDecFrameBuffering <= maxNumRefFrames; }
    bool operator==(const SpsReordering &) const = default;
};

// An SPS is a whole NAL unit, its header byte included, without a start code. None for one
// that cannot be read, which the official client leaves as it is.
[[nodiscard]] std::optional<SpsReordering> spsReordering(std::span<const uint8_t> sps);

// The SPS as the official client would rewrite it: a restriction with no reordering and as
// many pictures held as are referred to, in a VUI that says nothing else when it had to be
// added. Everything the SPS said before that is kept bit for bit. None when it is kept as it is.
[[nodiscard]] std::optional<std::vector<uint8_t>> rewrittenSps(std::span<const uint8_t> sps);

// Rewrites every SPS of an Annex B access unit that the official client would, and leaves
// every other byte of it alone. Whether one was rewritten.
bool rewriteSpsIn(std::vector<uint8_t> &accessUnit);

// The bytes of a NAL unit without the 03 that follows two zeros in it, and with one put back
// wherever a start code would otherwise be read.
[[nodiscard]] std::vector<uint8_t> withoutEmulationPrevention(std::span<const uint8_t> nal);
[[nodiscard]] std::vector<uint8_t> withEmulationPrevention(std::span<const uint8_t> payload);

} // namespace H264
} // namespace Media
} // namespace Core
} // namespace Acheron
