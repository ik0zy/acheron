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
constexpr int REPORT_BLOCK_SIZE = 24;
constexpr int MAX_REPORT_BLOCKS = 31;
constexpr int HEADER_SIZE = 4;
// Header, sender, NTP time: as far into a sender report as is read.
constexpr int SENDER_REPORT_READ_SIZE = 16;
// Discord's inbound estimate, as its bytes come off the wire. The native module reads the
// four as one number, 0x31534944, and writes "DIS1" in its own byte order.
constexpr uint8_t INBOUND_ESTIMATE_NAME[4] = { 0x31, 0x53, 0x49, 0x44 };
// Header, sender, name, then a user id (8), the rate (3) and the loss (1).
constexpr int INBOUND_ESTIMATE_SIZE = 24;

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
            received.senderReports.push_back({ qFromBigEndian<uint32_t>(p + 4), (seconds << 16) | (fraction >> 16) });
            readReportBlocks(p, size, SENDER_REPORT_BLOCKS_AT, received.receptionReports);
        } else if (p[1] == TYPE_RECEIVER_REPORT) {
            readReportBlocks(p, size, RECEIVER_REPORT_BLOCKS_AT, received.receptionReports);
        } else if (p[1] == TYPE_APPLICATION && size >= INBOUND_ESTIMATE_SIZE && std::equal(p + 8, p + 12, INBOUND_ESTIMATE_NAME)) {
            // The rate is an 18-bit mantissa under a 6-bit exponent, as in REMB.
            const uint32_t rate = (static_cast<uint32_t>(p[20]) << 16) | (static_cast<uint32_t>(p[21]) << 8) | p[22];
            received.hasInboundEstimate = true;
            received.inboundBitsPerSecond = static_cast<uint64_t>(rate & 0x3FFFF) << (rate >> 18);
            received.inboundLoss = p[23] / 256.0;
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
