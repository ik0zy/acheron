#pragma once

#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "PacketBuffer.hpp"
#include "Rtcp.hpp"
#include "RtpPacket.hpp"
#include "VideoPacketizer.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// The one video stream a connection sends, from whole encoded frames to RTP packets ready for
// the transport cipher. It packetizes and numbers them, lets them out at a measured pace, keeps
// what went out for a while so that a lost packet can be sent again on the RTX stream
// (RFC 4588), and counts what the sender report states.
//
// Like VideoReceiver it keeps no clock and no timer of its own: the owner passes the time in,
// in microseconds from any origin, and calls startInterval() every PACE_INTERVAL_MS while
// anything is waiting.
class VideoSendStream
{
public:
    // Where each of the stream's counters starts. The transport-wide one numbers every video
    // packet of the connection, retransmissions included.
    struct Numbering
    {
        uint16_t sequence = 0;
        uint16_t rtxSequence = 0;
        uint16_t transportSequence = 0;
        uint16_t pictureId = 0;
    };

    struct Stats
    {
        // First transmissions and the payload bytes in them, as the sender report counts.
        quint64 packets = 0;
        quint64 bytes = 0;
        quint64 frames = 0;
        quint64 keyframes = 0;
        quint64 retransmitted = 0;
        quint64 nacksReceived = 0;
        // Every byte written for the transport: headers and retransmissions too.
        quint64 bytesWritten = 0;
        // From the server's reception reports. Negative until it has reported.
        double lossFraction = -1;
        int roundTripMs = -1;
    };

    VideoSendStream(uint32_t ssrc, uint32_t rtxSsrc, const Numbering &first);

    [[nodiscard]] uint32_t ssrc() const { return mediaSsrc; }
    [[nodiscard]] VideoCodec codec() const { return packetizer ? packetizer->codec() : VideoCodec::Unknown; }

    // Frames are refused until this has named a codec that can be packetized. Without a payload
    // type for retransmissions none are sent. A change of codec forgets what the old one left.
    void setCodec(VideoCodec codec, uint8_t payloadType, std::optional<uint8_t> rtxPayloadType);

    // One whole frame, end-to-end encrypted already if it is going to be.
    bool queueFrame(std::span<const uint8_t> frame, uint32_t rtpTimestamp, bool keyframe, qint64 nowUs);
    // Packets the server asks for again. Those still in the history go out ahead of new ones.
    void retransmit(std::span<const uint16_t> sequences, qint64 nowUs);
    // Forgets what has not gone out yet, and what was kept for sending again.
    void clear();

    [[nodiscard]] bool hasWaiting() const { return !queued.empty() || !resends.empty(); }
    // Allows the next share of what is waiting out: a fixed number of packets, or more when
    // that would not clear the backlog within DRAIN_INTERVALS.
    void startInterval();
    // Writes the next packet of this interval's share into `packet`, with nothing yet sealed:
    // the first CLEAR_SIZE bytes are those the transport cipher leaves readable.
    [[nodiscard]] bool writeNext(qint64 nowUs, PacketBuffer &packet);

    // Nothing is reported before the first frame, as there is no RTP time to state.
    [[nodiscard]] bool canReport() const { return framesQueued; }
    // `ntpTime` is the wall clock's reading at `nowUs`, as 32.32 fixed point.
    int writeSenderReport(std::span<uint8_t> out, qint64 nowUs, uint64_t ntpTime);
    void onReceptionReport(const ReceptionReport &report, qint64 nowUs);

    [[nodiscard]] const Stats &stats() const { return counters; }

    // The largest RTP packet, before the transport cipher adds its tag and nonce.
    static constexpr int MAX_PACKET_SIZE = 1200;
    static constexpr int HISTORY_MS = 1000;
    static constexpr int PACE_INTERVAL_MS = 5;
    static constexpr int PACKETS_PER_INTERVAL = 8;
    static constexpr int DRAIN_INTERVALS = 20;

    static constexpr int CLEAR_SIZE = RtpHeader::FIXED_SIZE + RtpHeader::EXTENSION_PREAMBLE_SIZE;
    // The header extensions every video packet carries, by the ids Discord gives them.
    static constexpr uint8_t TRANSMISSION_OFFSET_EXTENSION = 2;
    static constexpr uint8_t ABS_SEND_TIME_EXTENSION = 3;
    static constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;
    // Those three as one-byte-form elements, padded to whole words.
    static constexpr int EXTENSIONS_SIZE = 12;
    // A retransmission's payload opens with the sequence number the packet first had.
    static constexpr int RTX_HEADER_SIZE = 2;

private:
    struct Packet
    {
        std::vector<uint8_t> payload;
        uint32_t timestamp = 0;
        bool marker = false;
        // Given when it first goes out.
        uint16_t sequence = 0;
        qint64 sentUs = 0;
    };

    struct SentReport
    {
        uint32_t compactNtp = 0;
        qint64 sentUs = 0;
    };

    [[nodiscard]] bool retransmits() const { return retransmissionSsrc != 0 && rtxPayloadType.has_value(); }
    [[nodiscard]] int maxPayloadSize() const;
    [[nodiscard]] const Packet *findSent(uint16_t sequence) const;
    void forgetOld(qint64 nowUs);
    bool write(PacketBuffer &out, const Packet &packet, bool retransmission, qint64 nowUs);

    uint32_t mediaSsrc;
    uint32_t retransmissionSsrc;
    uint8_t payloadType = 0;
    std::optional<uint8_t> rtxPayloadType;
    std::optional<VideoPacketizer> packetizer;

    uint16_t nextSequence;
    uint16_t nextRtxSequence;
    uint16_t nextTransportSequence;
    uint16_t firstPictureId;

    std::deque<Packet> queued;
    std::deque<uint16_t> resends;
    // In sequence order with no gaps, oldest first.
    std::deque<Packet> history;
    int intervalAllowance = 0;

    bool framesQueued = false;
    uint32_t lastFrameTimestamp = 0;
    qint64 lastFrameQueuedUs = 0;
    std::deque<SentReport> sentReports;

    Stats counters;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
