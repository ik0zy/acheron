#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

// One report block of a receiver report (RFC 3550, 6.4.1).
struct ReceptionReport
{
    uint32_t ssrc = 0;
    uint8_t fractionLost = 0;
    int32_t cumulativeLost = 0;
    uint32_t highestSequence = 0;
    uint32_t jitter = 0;
    uint32_t lastSenderReport = 0;
    uint32_t delaySinceLastSenderReport = 0;
};

// Each writer appends one RTCP packet at the start of `out` and returns its size, or -1 when it
// does not fit. A compound packet is these written back to back, receiver report first.
int writeReceiverReport(std::span<uint8_t> out, uint32_t senderSsrc, std::span<const ReceptionReport> reports);

// Generic NACK (RFC 4585, 6.2.1). `sequences` must be in ascending order, modulo wrap-around.
int writeNack(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc, std::span<const uint16_t> sequences);

// Picture loss indication (RFC 4585, 6.3.1): asks the sender for a key frame.
int writePli(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc);

// What a receiver takes from the control packets the server sends it.
struct ReceivedRtcp
{
    struct SenderReport
    {
        uint32_t ssrc = 0;
        // The middle 32 bits of the report's NTP time, which a receiver report echoes back
        // so that the sender can work out the round trip.
        uint32_t compactNtp = 0;
    };
    std::vector<SenderReport> senderReports;

    // What the server says it has received of the streams this client sends.
    std::vector<ReceptionReport> receptionReports;

    // The server's own estimate of what the way to this client carries, in bits per second,
    // and the share of packets it sees lost on it, 0 to 1. Discord sends this in an
    // application-defined packet; the official client only keeps the number for its statistics.
    bool hasInboundEstimate = false;
    uint64_t inboundBitsPerSecond = 0;
    double inboundLoss = 0;
};

// Reads a compound packet. Packets of other kinds are passed over, and reading stops at
// anything malformed.
[[nodiscard]] ReceivedRtcp parseRtcp(std::span<const uint8_t> compound);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
