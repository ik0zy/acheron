#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "VideoDepacketizer.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// Cuts the encoded frames of one video stream into RTP payloads; assembleFrame() is its reverse.
// H264 goes out in packetization mode 1 (RFC 6184) and VP8 under the RFC 7741 descriptor, each
// the way WebRTC writes it. The other codecs are not sent yet.
class VideoPacketizer
{
public:
    [[nodiscard]] static bool supports(VideoCodec codec);

    // Only VP8 numbers its pictures.
    explicit VideoPacketizer(VideoCodec codec, uint16_t firstPictureId = 0);

    [[nodiscard]] VideoCodec codec() const { return videoCodec; }

    // One frame's payloads in sending order, none larger than `maxPayloadSize`; the packet of
    // the last one carries the marker. An H264 frame is Annex B with start codes of either
    // length. Nothing comes back for a frame that cannot be packetized.
    [[nodiscard]] std::vector<std::vector<uint8_t>> packetize(std::span<const uint8_t> frame, int maxPayloadSize);

    // The smallest `maxPayloadSize` there is any sense in.
    static constexpr int MIN_PAYLOAD_SIZE = 16;

private:
    VideoCodec videoCodec;
    uint16_t pictureId;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
