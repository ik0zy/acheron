#include "Rtcp.hpp"

#include <QtEndian>

#include <algorithm>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr uint8_t RTCP_VERSION_BITS = 2 << 6;
constexpr uint8_t TYPE_RECEIVER_REPORT = 201;
constexpr uint8_t TYPE_TRANSPORT_FEEDBACK = 205;
constexpr uint8_t TYPE_PAYLOAD_FEEDBACK = 206;
constexpr uint8_t FORMAT_NACK = 1;
constexpr uint8_t FORMAT_PLI = 1;
constexpr int REPORT_BLOCK_SIZE = 24;
constexpr int MAX_REPORT_BLOCKS = 31;

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
