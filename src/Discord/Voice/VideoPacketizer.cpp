#include "VideoPacketizer.hpp"

#include <algorithm>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

using Bytes = std::span<const uint8_t>;
using Payloads = std::vector<std::vector<uint8_t>>;

// WebRTC cuts what does not fit into pieces of about one size, the longer ones last, and not
// into full packets and a remainder.
std::vector<size_t> aboutEqualSizes(size_t total, size_t room)
{
    const size_t count = (total + room - 1) / room;
    const size_t shorter = total / count;
    const size_t longerOnes = total % count;

    std::vector<size_t> sizes(count, shorter);
    for (size_t i = count - longerOnes; i < count; i++)
        sizes[i]++;
    return sizes;
}

// RFC 6184
constexpr uint8_t H264_TYPE_MASK = 0x1F;
constexpr uint8_t H264_IMPORTANCE_MASK = 0x60;
constexpr uint8_t H264_STAP_A = 24;
constexpr uint8_t H264_FU_A = 28;
constexpr uint8_t FU_START = 0x80;
constexpr uint8_t FU_END = 0x40;
constexpr size_t FU_HEADER_SIZE = 2;
constexpr size_t STAP_HEADER_SIZE = 1;
constexpr size_t STAP_LENGTH_SIZE = 2;

// Annex B: every NAL follows a start code of 00 00 01, which may have one more zero in front.
std::vector<Bytes> splitAnnexB(Bytes frame)
{
    constexpr size_t START_CODE_SIZE = 3;

    std::vector<Bytes> nals;
    size_t nalStart = 0;
    bool inNal = false;

    const auto endNal = [&](size_t end) {
        if (inNal && end > nalStart)
            nals.push_back(frame.subspan(nalStart, end - nalStart));
    };

    size_t i = 0;
    while (i + START_CODE_SIZE <= frame.size()) {
        if (frame[i] != 0 || frame[i + 1] != 0 || frame[i + 2] != 1) {
            i++;
            continue;
        }
        const bool longStartCode = i > nalStart && frame[i - 1] == 0;
        endNal(longStartCode ? i - 1 : i);
        i += START_CODE_SIZE;
        nalStart = i;
        inNal = true;
    }
    endNal(frame.size());
    return nals;
}

void appendFragments(Bytes nal, size_t maxPayloadSize, Payloads &payloads)
{
    const uint8_t indicator = static_cast<uint8_t>((nal[0] & ~H264_TYPE_MASK) | H264_FU_A);
    const uint8_t type = nal[0] & H264_TYPE_MASK;
    Bytes body = nal.subspan(1);

    const std::vector<size_t> sizes = aboutEqualSizes(body.size(), maxPayloadSize - FU_HEADER_SIZE);
    for (size_t i = 0; i < sizes.size(); i++) {
        const uint8_t position = static_cast<uint8_t>((i == 0 ? FU_START : 0) | (i + 1 == sizes.size() ? FU_END : 0));

        std::vector<uint8_t> &payload = payloads.emplace_back();
        payload.reserve(FU_HEADER_SIZE + sizes[i]);
        payload.push_back(indicator);
        payload.push_back(static_cast<uint8_t>(position | type));
        payload.insert(payload.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(sizes[i]));
        body = body.subspan(sizes[i]);
    }
}

void appendAggregate(std::span<const Bytes> nals, Payloads &payloads)
{
    // The aggregate is as important as the most important unit in it.
    uint8_t importance = 0;
    for (Bytes nal : nals)
        importance = std::max<uint8_t>(importance, nal[0] & H264_IMPORTANCE_MASK);

    std::vector<uint8_t> &payload = payloads.emplace_back();
    payload.push_back(static_cast<uint8_t>(importance | H264_STAP_A));
    for (Bytes nal : nals) {
        payload.push_back(static_cast<uint8_t>(nal.size() >> 8));
        payload.push_back(static_cast<uint8_t>(nal.size()));
        payload.insert(payload.end(), nal.begin(), nal.end());
    }
}

Payloads packetizeH264(Bytes frame, size_t maxPayloadSize)
{
    const std::vector<Bytes> nals = splitAnnexB(frame);

    Payloads payloads;
    size_t i = 0;
    while (i < nals.size()) {
        if (nals[i].size() > maxPayloadSize) {
            appendFragments(nals[i], maxPayloadSize, payloads);
            i++;
            continue;
        }

        // A unit that fits travels with as many of those after it as fit beside it, which is
        // what keeps a key frame's parameter sets in one packet.
        size_t count = 1;
        size_t aggregateSize = STAP_HEADER_SIZE + STAP_LENGTH_SIZE + nals[i].size();
        while (i + count < nals.size() && aggregateSize + STAP_LENGTH_SIZE + nals[i + count].size() <= maxPayloadSize) {
            aggregateSize += STAP_LENGTH_SIZE + nals[i + count].size();
            count++;
        }

        if (count == 1)
            payloads.emplace_back(nals[i].begin(), nals[i].end());
        else
            appendAggregate(std::span(nals).subspan(i, count), payloads);
        i += count;
    }
    return payloads;
}

// RFC 7741, 4.2. With one temporal layer WebRTC sends the extension byte for the sake of the
// picture id alone, and always in its 15-bit form.
constexpr uint8_t VP8_EXTENDED = 0x80;
constexpr uint8_t VP8_PARTITION_START = 0x10;
constexpr uint8_t VP8_HAS_PICTURE_ID = 0x80;
constexpr uint8_t VP8_LONG_PICTURE_ID = 0x80;
constexpr uint16_t VP8_PICTURE_ID_MASK = 0x7FFF;
constexpr size_t VP8_DESCRIPTOR_SIZE = 4;

Payloads packetizeVp8(Bytes frame, size_t maxPayloadSize, uint16_t pictureId)
{
    Payloads payloads;
    if (frame.empty())
        return payloads;

    const std::vector<size_t> sizes = aboutEqualSizes(frame.size(), maxPayloadSize - VP8_DESCRIPTOR_SIZE);
    for (size_t i = 0; i < sizes.size(); i++) {
        std::vector<uint8_t> &payload = payloads.emplace_back();
        payload.reserve(VP8_DESCRIPTOR_SIZE + sizes[i]);
        payload.push_back(static_cast<uint8_t>(VP8_EXTENDED | (i == 0 ? VP8_PARTITION_START : 0)));
        payload.push_back(VP8_HAS_PICTURE_ID);
        payload.push_back(static_cast<uint8_t>(VP8_LONG_PICTURE_ID | (pictureId >> 8)));
        payload.push_back(static_cast<uint8_t>(pictureId));
        payload.insert(payload.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(sizes[i]));
        frame = frame.subspan(sizes[i]);
    }
    return payloads;
}

} // namespace

bool VideoPacketizer::supports(VideoCodec codec)
{
    return codec == VideoCodec::H264 || codec == VideoCodec::VP8;
}

VideoPacketizer::VideoPacketizer(VideoCodec codec, uint16_t firstPictureId)
    : videoCodec(codec), pictureId(firstPictureId & VP8_PICTURE_ID_MASK)
{
}

std::vector<std::vector<uint8_t>> VideoPacketizer::packetize(std::span<const uint8_t> frame, int maxPayloadSize)
{
    if (maxPayloadSize < MIN_PAYLOAD_SIZE)
        return {};

    switch (videoCodec) {
    case VideoCodec::H264:
        return packetizeH264(frame, static_cast<size_t>(maxPayloadSize));
    case VideoCodec::VP8: {
        Payloads payloads = packetizeVp8(frame, static_cast<size_t>(maxPayloadSize), pictureId);
        if (!payloads.empty())
            pictureId = (pictureId + 1) & VP8_PICTURE_ID_MASK;
        return payloads;
    }
    case VideoCodec::H265:
    case VideoCodec::AV1:
    case VideoCodec::Unknown:
        break;
    }
    return {};
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
