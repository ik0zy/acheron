#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "VideoDepacketizer.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// Cuts the encoded frames of one video stream into RTP payloads; assembleFrame() is its reverse.
// Each codec goes out the way WebRTC writes it: H264 in packetization mode 1 (RFC 6184), H265
// without DONL fields (RFC 7798), VP8 under the RFC 7741 descriptor, and AV1 as OBU elements
// behind an aggregation header (https://aomediacodec.github.io/av1-rtp-spec/).
class VideoPacketizer
{
public:
    // Every codec there is a name for.
    [[nodiscard]] static bool supports(VideoCodec codec);

    // Only VP8 numbers its pictures.
    explicit VideoPacketizer(VideoCodec codec, uint16_t firstPictureId = 0);

    [[nodiscard]] VideoCodec codec() const { return videoCodec; }

    // One frame's payloads in sending order, none larger than `maxPayloadSize`; the packet of
    // the last one carries the marker. An H264 or H265 frame is Annex B with start codes of
    // either length. An AV1 frame is a run of OBUs of which only the last may be without its
    // size field, which is how the end-to-end encryptor leaves it; temporal delimiters, tile
    // lists and padding are not sent. `keyframe` matters to AV1 alone, whose first packet says
    // whether a new coded video sequence begins. Nothing comes back for a frame that cannot be
    // packetized.
    [[nodiscard]] std::vector<std::vector<uint8_t>> packetize(std::span<const uint8_t> frame, bool keyframe, int maxPayloadSize);

    // The smallest `maxPayloadSize` there is any sense in.
    static constexpr int MIN_PAYLOAD_SIZE = 16;

private:
    VideoCodec videoCodec;
    uint16_t pictureId;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
