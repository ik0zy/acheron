#include "Rtcp.hpp"

#include <QtEndian>

#include <algorithm>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr uint8_t RTCP_VERSION_BITS = 2 << 6;
constexpr uint8_t TYPE_SENDER_REPORT = 200;
constexpr uint8_t TYPE_RECEIVER_REPORT = 201;
constexpr uint8_t TYPE_APPLICATION = 204;
constexpr uint8_t TYPE_TRANSPORT_FEEDBACK = 205;
constexpr uint8_t TYPE_PAYLOAD_FEEDBACK = 206;
constexpr uint8_t FORMAT_NACK = 1;
constexpr uint8_t FORMAT_PLI = 1;
constexpr uint8_t FORMAT_FIR = 4;
constexpr uint8_t FORMAT_APPLICATION_LAYER = 15;
constexpr uint8_t RECEIVER_ESTIMATE_NAME[4] = { 'R', 'E', 'M', 'B' };
// Header, sender, an unused media source, the name, then a count of SSRCs (1) and the rate (3).
constexpr int RECEIVER_ESTIMATE_SIZE = 20;
constexpr int REPORT_BLOCK_SIZE = 24;
constexpr int MAX_REPORT_BLOCKS = 31;
constexpr int HEADER_SIZE = 4;
// Header, sender, NTP time: as much of a sender report as has to be there.
constexpr int SENDER_REPORT_READ_SIZE = 16;
// With the RTP timestamp and the two counts that follow.
constexpr int SENDER_REPORT_SIZE = 28;
// Header, sender and media source: what every feedback packet opens with.
constexpr int FEEDBACK_HEADER_SIZE = 12;
// Discord's inbound estimate, as its bytes come off the wire. The native module reads the
// four as one number, 0x31534944, and writes "DIS1" in its own byte order.
constexpr uint8_t INBOUND_ESTIMATE_NAME[4] = { 0x31, 0x53, 0x49, 0x44 };
// Header, sender, name, then a user id (8), the rate (3) and the loss (1).
constexpr int INBOUND_ESTIMATE_SIZE = 24;
// Its sibling, "DIS0": who is receiving one of this client's video streams. The stream's SSRC
// is where a sender's would be, the number of receivers is in the header's five count bits, and
// for each of them follows an entry of a user id (8), a rate (3) and one more byte.
constexpr uint8_t RECEIVER_COUNT_NAME[4] = { 0x30, 0x53, 0x49, 0x44 };
// And "DISX": the SSRC where a sender's would be is one of the remote sender's, and nothing
// after the name is read.
constexpr uint8_t VIDEO_RESET_NAME[4] = { 0x58, 0x53, 0x49, 0x44 };
constexpr int APPLICATION_HEADER_SIZE = 12;

// The length field counts 32-bit words, not including the first.
void writeHeader(uint8_t *p, uint8_t countOrFormat, uint8_t type, int size)
{
    p[0] = RTCP_VERSION_BITS | countOrFormat;
    p[1] = type;
    qToBigEndian(static_cast<uint16_t>(size / 4 - 1), p + 2);
}

} // namespace

int writeReceiverReport(std::span<uint8_t> out, uint32_t senderSsrc, std::span<const ReceptionReport> reports)
{
    const int count = static_cast<int>(std::min<size_t>(reports.size(), MAX_REPORT_BLOCKS));
    const int size = 8 + count * REPORT_BLOCK_SIZE;
    if (static_cast<int>(out.size()) < size)
        return -1;

    uint8_t *p = out.data();
    writeHeader(p, static_cast<uint8_t>(count), TYPE_RECEIVER_REPORT, size);
    qToBigEndian(senderSsrc, p + 4);
    p += 8;

    for (int i = 0; i < count; i++) {
        const ReceptionReport &report = reports[i];
        const int32_t lost = std::clamp(report.cumulativeLost, -0x800000, 0x7FFFFF);
        qToBigEndian(report.ssrc, p);
        p[4] = report.fractionLost;
        p[5] = static_cast<uint8_t>(lost >> 16);
        p[6] = static_cast<uint8_t>(lost >> 8);
        p[7] = static_cast<uint8_t>(lost);
        qToBigEndian(report.highestSequence, p + 8);
        qToBigEndian(report.jitter, p + 12);
        qToBigEndian(report.lastSenderReport, p + 16);
        qToBigEndian(report.delaySinceLastSenderReport, p + 20);
        p += REPORT_BLOCK_SIZE;
    }
    return size;
}

int writeSenderReport(std::span<uint8_t> out, const SenderInfo &info)
{
    if (static_cast<int>(out.size()) < SENDER_REPORT_SIZE)
        return -1;

    uint8_t *p = out.data();
    writeHeader(p, 0, TYPE_SENDER_REPORT, SENDER_REPORT_SIZE);
    qToBigEndian(info.ssrc, p + 4);
    qToBigEndian(static_cast<quint64>(info.ntpTime), p + 8);
    qToBigEndian(info.rtpTimestamp, p + 16);
    qToBigEndian(info.packetCount, p + 20);
    qToBigEndian(info.octetCount, p + 24);
    return SENDER_REPORT_SIZE;
}

static void readNack(const uint8_t *packet, size_t size, std::vector<ReceivedRtcp::Nack> &nacks)
{
    ReceivedRtcp::Nack nack;
    nack.mediaSsrc = qFromBigEndian<uint32_t>(packet + 8);
    for (size_t at = FEEDBACK_HEADER_SIZE; at + 4 <= size; at += 4) {
        const uint16_t first = qFromBigEndian<uint16_t>(packet + at);
        const uint16_t following = qFromBigEndian<uint16_t>(packet + at + 2);
        nack.sequences.push_back(first);
        for (int bit = 0; bit < 16; bit++) {
            if (following & (1u << bit))
                nack.sequences.push_back(static_cast<uint16_t>(first + bit + 1));
        }
    }
    if (!nack.sequences.empty())
        nacks.push_back(std::move(nack));
}

// A full intra request (RFC 5104, 4.3.1) names its streams in entries of an SSRC, a request
// number and three unused bytes; the media source in its header is left at zero.
static void readFullIntraRequest(const uint8_t *packet, size_t size, std::vector<uint32_t> &ssrcs)
{
    constexpr size_t ENTRY_SIZE = 8;
    for (size_t at = FEEDBACK_HEADER_SIZE; at + ENTRY_SIZE <= size; at += ENTRY_SIZE)
        ssrcs.push_back(qFromBigEndian<uint32_t>(packet + at));
}

static void readReportBlocks(const uint8_t *packet, size_t size, size_t blocksAt, std::vector<ReceptionReport> &reports)
{
    const int count = packet[0] & 0x1F;
    for (int i = 0; i < count && blocksAt + REPORT_BLOCK_SIZE <= size; i++, blocksAt += REPORT_BLOCK_SIZE) {
        const uint8_t *p = packet + blocksAt;
        ReceptionReport report;
        report.ssrc = qFromBigEndian<uint32_t>(p);
        report.fractionLost = p[4];
        // A 24-bit signed number.
        report.cumulativeLost = static_cast<int32_t>((static_cast<uint32_t>(p[5]) << 24) | (static_cast<uint32_t>(p[6]) << 16) | (static_cast<uint32_t>(p[7]) << 8)) >> 8;
        report.highestSequence = qFromBigEndian<uint32_t>(p + 8);
        report.jitter = qFromBigEndian<uint32_t>(p + 12);
        report.lastSenderReport = qFromBigEndian<uint32_t>(p + 16);
        report.delaySinceLastSenderReport = qFromBigEndian<uint32_t>(p + 20);
        reports.push_back(report);
    }
}

static void readReceiverEstimate(const uint8_t *p, size_t size, ReceivedRtcp &received)
{
    if (size < RECEIVER_ESTIMATE_SIZE || size < RECEIVER_ESTIMATE_SIZE + size_t(4) * p[16] || !std::equal(p + 12, p + 16, RECEIVER_ESTIMATE_NAME))
        return;

    // An 18-bit mantissa under a 6-bit exponent, which can say more than 64 bits hold.
    const uint64_t mantissa = (static_cast<uint64_t>(p[17] & 0x03) << 16) | (static_cast<uint64_t>(p[18]) << 8) | p[19];
    const int exponent = p[17] >> 2;
    const uint64_t rate = mantissa << exponent;
    if (rate >> exponent != mantissa)
        return;
    received.hasReceiverEstimate = true;
    received.receiverEstimateBitsPerSecond = std::max(received.receiverEstimateBitsPerSecond, rate);
}

ReceivedRtcp parseRtcp(std::span<const uint8_t> compound)
{
    constexpr size_t SENDER_REPORT_BLOCKS_AT = 28;
    constexpr size_t RECEIVER_REPORT_BLOCKS_AT = 8;

    ReceivedRtcp received;
    while (compound.size() >= HEADER_SIZE) {
        const uint8_t *p = compound.data();
        const size_t size = (static_cast<size_t>(qFromBigEndian<uint16_t>(p + 2)) + 1) * 4;
        if ((p[0] & 0xC0) != RTCP_VERSION_BITS || size > compound.size())
            break;

        if (p[1] == TYPE_SENDER_REPORT && size >= SENDER_REPORT_READ_SIZE) {
            const uint32_t seconds = qFromBigEndian<uint32_t>(p + 8);
            const uint32_t fraction = qFromBigEndian<uint32_t>(p + 12);
            ReceivedRtcp::SenderReport report;
            report.ssrc = qFromBigEndian<uint32_t>(p + 4);
            report.compactNtp = (seconds << 16) | (fraction >> 16);
            if (size >= SENDER_REPORT_SIZE) {
                report.rtpTimestamp = qFromBigEndian<uint32_t>(p + 16);
                report.packetCount = qFromBigEndian<uint32_t>(p + 20);
                report.octetCount = qFromBigEndian<uint32_t>(p + 24);
            }
            received.senderReports.push_back(report);
            readReportBlocks(p, size, SENDER_REPORT_BLOCKS_AT, received.receptionReports);
        } else if (p[1] == TYPE_RECEIVER_REPORT) {
            readReportBlocks(p, size, RECEIVER_REPORT_BLOCKS_AT, received.receptionReports);
        } else if (p[1] == TYPE_APPLICATION && size >= INBOUND_ESTIMATE_SIZE && std::equal(p + 8, p + 12, INBOUND_ESTIMATE_NAME)) {
            // The rate is an 18-bit mantissa under a 6-bit exponent, as in REMB.
            const uint32_t rate = (static_cast<uint32_t>(p[20]) << 16) | (static_cast<uint32_t>(p[21]) << 8) | p[22];
            received.hasInboundEstimate = true;
            received.inboundBitsPerSecond = static_cast<uint64_t>(rate & 0x3FFFF) << (rate >> 18);
            received.inboundLoss = p[23] / 256.0;
        } else if (p[1] == TYPE_APPLICATION && size >= APPLICATION_HEADER_SIZE && std::equal(p + 8, p + 12, RECEIVER_COUNT_NAME)) {
            received.receiverCounts.push_back({ qFromBigEndian<uint32_t>(p + 4), p[0] & 0x1F });
        } else if (p[1] == TYPE_APPLICATION && size >= APPLICATION_HEADER_SIZE && std::equal(p + 8, p + 12, VIDEO_RESET_NAME)) {
            received.videoResets.push_back(qFromBigEndian<uint32_t>(p + 4));
        } else if (p[1] == TYPE_PAYLOAD_FEEDBACK && (p[0] & 0x1F) == FORMAT_APPLICATION_LAYER) {
            readReceiverEstimate(p, size, received);
        } else if (p[1] == TYPE_TRANSPORT_FEEDBACK && (p[0] & 0x1F) == FORMAT_NACK && size >= FEEDBACK_HEADER_SIZE) {
            readNack(p, size, received.nacks);
        } else if (p[1] == TYPE_PAYLOAD_FEEDBACK && (p[0] & 0x1F) == FORMAT_PLI && size >= FEEDBACK_HEADER_SIZE) {
            received.keyframeRequests.push_back(qFromBigEndian<uint32_t>(p + 8));
        } else if (p[1] == TYPE_PAYLOAD_FEEDBACK && (p[0] & 0x1F) == FORMAT_FIR && size >= FEEDBACK_HEADER_SIZE) {
            readFullIntraRequest(p, size, received.keyframeRequests);
        }
        compound = compound.subspan(size);
    }
    return received;
}

int writeNack(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc, std::span<const uint16_t> sequences)
{
    if (sequences.empty() || out.size() < 12)
        return -1;

    uint8_t *p = out.data();
    int size = 12;
    size_t i = 0;
    while (i < sequences.size()) {
        if (static_cast<int>(out.size()) < size + 4)
            return -1;

        // One entry names a packet and, as a bitmask, any of the sixteen after it.
        const uint16_t first = sequences[i++];
        uint16_t following = 0;
        while (i < sequences.size()) {
            const uint16_t distance = static_cast<uint16_t>(sequences[i] - first);
            if (distance == 0 || distance > 16)
                break;
            following |= static_cast<uint16_t>(1u << (distance - 1));
            i++;
        }
        qToBigEndian(first, p + size);
        qToBigEndian(following, p + size + 2);
        size += 4;
    }

    writeHeader(p, FORMAT_NACK, TYPE_TRANSPORT_FEEDBACK, size);
    qToBigEndian(senderSsrc, p + 4);
    qToBigEndian(mediaSsrc, p + 8);
    return size;
}

int writePli(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc)
{
    constexpr int SIZE = 12;
    if (static_cast<int>(out.size()) < SIZE)
        return -1;

    uint8_t *p = out.data();
    writeHeader(p, FORMAT_PLI, TYPE_PAYLOAD_FEEDBACK, SIZE);
    qToBigEndian(senderSsrc, p + 4);
    qToBigEndian(mediaSsrc, p + 8);
    return SIZE;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
