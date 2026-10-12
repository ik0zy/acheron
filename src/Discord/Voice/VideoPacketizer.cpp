#include "VideoPacketizer.hpp"

#include <algorithm>
#include <array>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

using Bytes = std::span<const uint8_t>;
using Payloads = std::vector<std::vector<uint8_t>>;

void append(std::vector<uint8_t> &out, Bytes bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

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

// What RFC 6184 and RFC 7798 have in common.
constexpr uint8_t FU_START = 0x80;
constexpr uint8_t FU_END = 0x40;
constexpr size_t AGGREGATED_LENGTH_SIZE = 2;

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

// A unit that does not fit, as pieces that each open with `header` and a byte that marks the
// first and the last of them over the unit's type.
void appendFragments(Bytes header, uint8_t type, Bytes body, size_t maxPayloadSize, Payloads &payloads)
{
    const size_t headerSize = header.size() + 1;
    const std::vector<size_t> sizes = aboutEqualSizes(body.size(), maxPayloadSize - headerSize);
    for (size_t i = 0; i < sizes.size(); i++) {
        const uint8_t position = static_cast<uint8_t>((i == 0 ? FU_START : 0) | (i + 1 == sizes.size() ? FU_END : 0));

        std::vector<uint8_t> &payload = payloads.emplace_back();
        payload.reserve(headerSize + sizes[i]);
        append(payload, header);
        payload.push_back(static_cast<uint8_t>(position | type));
        append(payload, body.first(sizes[i]));
        body = body.subspan(sizes[i]);
    }
}

void appendAggregate(Bytes header, std::span<const Bytes> nals, Payloads &payloads)
{
    std::vector<uint8_t> &payload = payloads.emplace_back();
    append(payload, header);
    for (Bytes nal : nals) {
        payload.push_back(static_cast<uint8_t>(nal.size() >> 8));
        payload.push_back(static_cast<uint8_t>(nal.size()));
        append(payload, nal);
    }
}

// How one of the two NAL codecs fragments a unit and heads an aggregate.
struct NalFormat
{
    size_t nalHeaderSize;
    size_t aggregateHeaderSize;
    void (*fragment)(Bytes nal, size_t maxPayloadSize, Payloads &payloads);
    void (*aggregate)(std::span<const Bytes> nals, Payloads &payloads);
};

Payloads packetizeNals(Bytes frame, size_t maxPayloadSize, const NalFormat &format)
{
    const std::vector<Bytes> nals = splitAnnexB(frame);
    if (std::any_of(nals.begin(), nals.end(), [&format](Bytes nal) { return nal.size() < format.nalHeaderSize; }))
        return {};

    Payloads payloads;
    size_t i = 0;
    while (i < nals.size()) {
        if (nals[i].size() > maxPayloadSize) {
            format.fragment(nals[i], maxPayloadSize, payloads);
            i++;
            continue;
        }

        // A unit that fits travels with as many of those after it as fit beside it, which is
        // what keeps a key frame's parameter sets in one packet.
        size_t count = 1;
        size_t aggregateSize = format.aggregateHeaderSize + AGGREGATED_LENGTH_SIZE + nals[i].size();
        while (i + count < nals.size() && aggregateSize + AGGREGATED_LENGTH_SIZE + nals[i + count].size() <= maxPayloadSize) {
            aggregateSize += AGGREGATED_LENGTH_SIZE + nals[i + count].size();
            count++;
        }

        if (count == 1)
            payloads.emplace_back(nals[i].begin(), nals[i].end());
        else
            format.aggregate(std::span(nals).subspan(i, count), payloads);
        i += count;
    }
    return payloads;
}

// RFC 6184
constexpr uint8_t H264_TYPE_MASK = 0x1F;
constexpr uint8_t H264_IMPORTANCE_MASK = 0x60;
constexpr uint8_t H264_STAP_A = 24;
constexpr uint8_t H264_FU_A = 28;

void appendH264Fragments(Bytes nal, size_t maxPayloadSize, Payloads &payloads)
{
    const uint8_t indicator = static_cast<uint8_t>((nal[0] & ~H264_TYPE_MASK) | H264_FU_A);
    appendFragments({ &indicator, 1 }, nal[0] & H264_TYPE_MASK, nal.subspan(1), maxPayloadSize, payloads);
}

void appendH264Aggregate(std::span<const Bytes> nals, Payloads &payloads)
{
    // The aggregate is as important as the most important unit in it.
    uint8_t importance = 0;
    for (Bytes nal : nals)
        importance = std::max<uint8_t>(importance, nal[0] & H264_IMPORTANCE_MASK);

    const uint8_t header = static_cast<uint8_t>(importance | H264_STAP_A);
    appendAggregate({ &header, 1 }, nals, payloads);
}

constexpr NalFormat H264_FORMAT = { 1, 1, appendH264Fragments, appendH264Aggregate };

// RFC 7798. A unit's two header bytes hold a forbidden bit, six bits of type, six of layer id
// and three of temporal id.
constexpr uint8_t H265_AP = 48;
constexpr uint8_t H265_FU = 49;
constexpr uint8_t H265_TYPE_BITS = 0x7E;
constexpr uint8_t H265_HIGHEST_LAYER_ID = 0x3F;
constexpr uint8_t H265_TEMPORAL_ID_MASK = 0x07;
constexpr size_t H265_NAL_HEADER_SIZE = 2;

void appendH265Fragments(Bytes nal, size_t maxPayloadSize, Payloads &payloads)
{
    const uint8_t type = (nal[0] & H265_TYPE_BITS) >> 1;
    const uint8_t header[] = { static_cast<uint8_t>((nal[0] & ~H265_TYPE_BITS) | (H265_FU << 1)), nal[1] };
    appendFragments(header, type, nal.subspan(H265_NAL_HEADER_SIZE), maxPayloadSize, payloads);
}

void appendH265Aggregate(std::span<const Bytes> nals, Payloads &payloads)
{
    // The aggregate is of the lowest layer and the lowest temporal layer among its units.
    uint8_t layerId = H265_HIGHEST_LAYER_ID;
    uint8_t temporalId = H265_TEMPORAL_ID_MASK;
    for (Bytes nal : nals) {
        layerId = std::min<uint8_t>(layerId, static_cast<uint8_t>(((nal[0] & 0x01) << 5) | (nal[1] >> 3)));
        temporalId = std::min<uint8_t>(temporalId, nal[1] & H265_TEMPORAL_ID_MASK);
    }

    const uint8_t header[] = { static_cast<uint8_t>((H265_AP << 1) | (layerId >> 5)), static_cast<uint8_t>((layerId << 3) | temporalId) };
    appendAggregate(header, nals, payloads);
}

constexpr NalFormat H265_FORMAT = { H265_NAL_HEADER_SIZE, H265_NAL_HEADER_SIZE, appendH265Fragments, appendH265Aggregate };

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

// https://aomediacodec.github.io/av1-rtp-spec/, laid out as WebRTC's rtp_packetizer_av1.cc does.
constexpr uint8_t AV1_CONTINUES_PREVIOUS = 0x80;
constexpr uint8_t AV1_CONTINUES_IN_NEXT = 0x40;
constexpr uint8_t AV1_NEW_SEQUENCE = 0x08;
constexpr int AV1_ELEMENT_COUNT_SHIFT = 4;
// The header counts up to three elements, and the last of those needs no length. More than
// three are not counted, and each of them carries its length.
constexpr size_t AV1_MOST_COUNTED_ELEMENTS = 3;
constexpr size_t AV1_AGGREGATION_HEADER_SIZE = 1;
constexpr uint8_t OBU_HAS_EXTENSION = 0x04;
constexpr uint8_t OBU_HAS_SIZE = 0x02;
constexpr uint8_t OBU_SEQUENCE_HEADER = 1;
constexpr uint8_t OBU_TEMPORAL_DELIMITER = 2;
constexpr uint8_t OBU_TILE_LIST = 8;
constexpr uint8_t OBU_PADDING = 15;
constexpr int LEB128_MAX_BYTES = 8;

bool readLeb128(Bytes &bytes, size_t &value)
{
    value = 0;
    for (int i = 0; i < LEB128_MAX_BYTES; i++) {
        if (bytes.empty())
            return false;
        const uint8_t byte = bytes[0];
        bytes = bytes.subspan(1);
        value |= size_t(byte & 0x7F) << (7 * i);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

void appendLeb128(std::vector<uint8_t> &out, size_t value)
{
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

size_t leb128Size(size_t value)
{
    size_t size = 1;
    while (value >>= 7)
        size++;
    return size;
}

// The largest element that fits into `room` with its length in front of it.
size_t largestWithItsLength(size_t room)
{
    size_t size = room;
    while (size > 0 && size + leb128Size(size) > room)
        size--;
    return size;
}

// An OBU as it travels: its header without the bit that announces a size field, the extension
// byte if it has one, and its payload.
struct Obu
{
    std::array<uint8_t, 2> header;
    size_t headerSize;
    Bytes body;

    [[nodiscard]] size_t size() const { return headerSize + body.size(); }
    [[nodiscard]] uint8_t type() const { return (header[0] >> 3) & 0x0F; }
};

// An encoder gives every OBU its size. The end-to-end encryptor takes the last one's away and
// puts its trailer behind that OBU's payload, so an OBU without a size runs to the frame's end.
bool splitObus(Bytes frame, std::vector<Obu> &obus)
{
    while (!frame.empty()) {
        const bool hasSize = frame[0] & OBU_HAS_SIZE;

        Obu obu;
        obu.headerSize = (frame[0] & OBU_HAS_EXTENSION) ? 2 : 1;
        if (frame.size() < obu.headerSize)
            return false;
        obu.header = { static_cast<uint8_t>(frame[0] & ~OBU_HAS_SIZE), obu.headerSize == 2 ? frame[1] : uint8_t(0) };
        frame = frame.subspan(obu.headerSize);

        size_t size = frame.size();
        if (hasSize && (!readLeb128(frame, size) || size > frame.size()))
            return false;
        obu.body = frame.first(size);
        frame = frame.subspan(size);

        if (obu.type() != OBU_TEMPORAL_DELIMITER && obu.type() != OBU_TILE_LIST && obu.type() != OBU_PADDING)
            obus.push_back(obu);
    }
    return true;
}

// `size` bytes of an OBU, from `offset` into it.
void appendPart(std::vector<uint8_t> &out, const Obu &obu, size_t offset, size_t size)
{
    const Bytes header = Bytes(obu.header).first(obu.headerSize);
    if (offset < header.size()) {
        const Bytes part = header.subspan(offset, std::min(size, header.size() - offset));
        append(out, part);
        size -= part.size();
        offset = header.size();
    }
    append(out, obu.body.subspan(offset - header.size(), size));
}

// The elements of one packet: all of `elements` OBUs from `firstObu` on, except that the first
// may begin `firstObuOffset` into its OBU and the last may end early, after `lastElementSize`.
struct Av1Packet
{
    size_t firstObu = 0;
    size_t firstObuOffset = 0;
    size_t elements = 0;
    size_t lastElementSize = 0;
    // Of everything behind the aggregation header.
    size_t size = 0;
};

// Fills each packet before the next is begun. An OBU that does not fit takes what is left of
// the packet, then whole packets, and its end opens a packet the next OBU may share.
std::vector<Av1Packet> layOutAv1(std::span<const Obu> obus, size_t room)
{
    std::vector<Av1Packet> packets(1);
    size_t remaining = room;
    for (size_t i = 0; i < obus.size(); i++) {
        const size_t obuSize = obus[i].size();

        // An element behind the packet's last one makes that one state its length, if it does
        // not already. The packet has to hold that and something of the new element too.
        const size_t held = packets.back().elements;
        size_t lengthOfPrevious = held == 0 || held > AV1_MOST_COUNTED_ELEMENTS ? 0 : leb128Size(packets.back().lastElementSize);
        const size_t leastWorthAdding = held >= AV1_MOST_COUNTED_ELEMENTS ? 2 : 1;
        if (remaining < lengthOfPrevious + leastWorthAdding) {
            packets.push_back({ i });
            remaining = room;
            lengthOfPrevious = 0;
        }

        Av1Packet &packet = packets.back();
        packet.size += lengthOfPrevious;
        remaining -= lengthOfPrevious;
        packet.elements++;
        const bool statesLength = packet.elements > AV1_MOST_COUNTED_ELEMENTS;

        const size_t whole = obuSize + (statesLength ? leb128Size(obuSize) : 0);
        if (whole <= remaining) {
            packet.lastElementSize = obuSize;
            packet.size += whole;
            remaining -= whole;
            continue;
        }

        const size_t first = std::min(obuSize - 1, statesLength ? largestWithItsLength(remaining) : remaining);
        packet.lastElementSize = first;
        packet.size += first + (statesLength ? leb128Size(first) : 0);

        size_t offset = first;
        for (; offset + room < obuSize; offset += room)
            packets.push_back({ i, offset, 1, room, room });
        const size_t last = obuSize - offset;
        packets.push_back({ i, offset, 1, last, last });
        remaining = room - last;
    }
    return packets;
}

std::vector<uint8_t> writeAv1Packet(const Av1Packet &packet, std::span<const Obu> obus, bool newSequence)
{
    const Obu &lastObu = obus[packet.firstObu + packet.elements - 1];
    const size_t lastElementOffset = packet.elements == 1 ? packet.firstObuOffset : 0;

    uint8_t aggregation = 0;
    if (packet.firstObuOffset > 0)
        aggregation |= AV1_CONTINUES_PREVIOUS;
    if (lastElementOffset + packet.lastElementSize < lastObu.size())
        aggregation |= AV1_CONTINUES_IN_NEXT;
    if (packet.elements <= AV1_MOST_COUNTED_ELEMENTS)
        aggregation |= static_cast<uint8_t>(packet.elements << AV1_ELEMENT_COUNT_SHIFT);
    if (newSequence)
        aggregation |= AV1_NEW_SEQUENCE;

    std::vector<uint8_t> payload;
    payload.reserve(AV1_AGGREGATION_HEADER_SIZE + packet.size);
    payload.push_back(aggregation);

    size_t offset = packet.firstObuOffset;
    for (size_t i = 0; i + 1 < packet.elements; i++) {
        const Obu &obu = obus[packet.firstObu + i];
        appendLeb128(payload, obu.size() - offset);
        appendPart(payload, obu, offset, obu.size() - offset);
        offset = 0;
    }
    if (packet.elements > AV1_MOST_COUNTED_ELEMENTS)
        appendLeb128(payload, packet.lastElementSize);
    appendPart(payload, lastObu, lastElementOffset, packet.lastElementSize);
    return payload;
}

Payloads packetizeAv1(Bytes frame, bool keyframe, size_t maxPayloadSize)
{
    std::vector<Obu> obus;
    if (!splitObus(frame, obus) || obus.empty())
        return {};

    const size_t room = maxPayloadSize - AV1_AGGREGATION_HEADER_SIZE;
    std::vector<Av1Packet> packets = layOutAv1(obus, room);
    if (packets.size() > 1) {
        // Filled one after the other, the packets end in a short one. WebRTC evens them out by
        // laying the frame out again in packets smaller by a share of the room left unused.
        size_t unused = 0;
        for (const Av1Packet &packet : packets)
            unused += room - packet.size;
        packets = layOutAv1(obus, room - unused / packets.size());
    }

    // As in WebRTC, an encoder's word that this is a key frame is not enough: a new sequence
    // is announced only when the frame brings its sequence header.
    const bool newSequence = keyframe && obus.front().type() == OBU_SEQUENCE_HEADER;

    Payloads payloads;
    for (size_t i = 0; i < packets.size(); i++)
        payloads.push_back(writeAv1Packet(packets[i], obus, newSequence && i == 0));
    return payloads;
}

} // namespace

bool VideoPacketizer::supports(VideoCodec codec)
{
    return codec != VideoCodec::Unknown;
}

VideoPacketizer::VideoPacketizer(VideoCodec codec, uint16_t firstPictureId)
    : videoCodec(codec), pictureId(firstPictureId & VP8_PICTURE_ID_MASK)
{
}

std::vector<std::vector<uint8_t>> VideoPacketizer::packetize(std::span<const uint8_t> frame, bool keyframe, int maxPayloadSize)
{
    if (maxPayloadSize < MIN_PAYLOAD_SIZE)
        return {};

    switch (videoCodec) {
    case VideoCodec::H264:
        return packetizeNals(frame, static_cast<size_t>(maxPayloadSize), H264_FORMAT);
    case VideoCodec::H265:
        return packetizeNals(frame, static_cast<size_t>(maxPayloadSize), H265_FORMAT);
    case VideoCodec::VP8: {
        Payloads payloads = packetizeVp8(frame, static_cast<size_t>(maxPayloadSize), pictureId);
        if (!payloads.empty())
            pictureId = (pictureId + 1) & VP8_PICTURE_ID_MASK;
        return payloads;
    }
    case VideoCodec::AV1:
        return packetizeAv1(frame, keyframe, static_cast<size_t>(maxPayloadSize));
    case VideoCodec::Unknown:
        break;
    }
    return {};
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
