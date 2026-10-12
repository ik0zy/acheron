#include "VideoSender.hpp"

#include <QtEndian>

#include <algorithm>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr int VIDEO_CLOCK_KHZ = 90;
constexpr qint64 HISTORY_US = qint64(VideoSendStream::HISTORY_MS) * 1000;
// A reception report answers one of the last few sender reports; older ones are of no use.
constexpr size_t REMEMBERED_REPORTS = 8;

} // namespace

VideoSendStream::VideoSendStream(uint32_t ssrc, uint32_t rtxSsrc, const Numbering &first)
    : mediaSsrc(ssrc), retransmissionSsrc(rtxSsrc), nextSequence(first.sequence), nextRtxSequence(first.rtxSequence), nextTransportSequence(first.transportSequence), firstPictureId(first.pictureId)
{
}

void VideoSendStream::setCodec(VideoCodec codec, uint8_t mediaPayloadType, std::optional<uint8_t> retransmissionPayloadType)
{
    if (this->codec() == codec && payloadType == mediaPayloadType && rtxPayloadType == retransmissionPayloadType)
        return;

    clear();
    payloadType = mediaPayloadType;
    rtxPayloadType = retransmissionPayloadType;
    packetizer.reset();
    if (VideoPacketizer::supports(codec))
        packetizer.emplace(codec, firstPictureId);
}

int VideoSendStream::maxPayloadSize() const
{
    // As WebRTC does, room is left in every packet for it to be sent again whole.
    return MAX_PACKET_SIZE - CLEAR_SIZE - EXTENSIONS_SIZE - (retransmits() ? RTX_HEADER_SIZE : 0);
}

bool VideoSendStream::queueFrame(std::span<const uint8_t> frame, uint32_t rtpTimestamp, bool keyframe, qint64 nowUs)
{
    if (!packetizer)
        return false;

    std::vector<std::vector<uint8_t>> payloads = packetizer->packetize(frame, maxPayloadSize());
    if (payloads.empty())
        return false;

    for (size_t i = 0; i < payloads.size(); i++) {
        Packet &packet = queued.emplace_back();
        packet.payload = std::move(payloads[i]);
        packet.timestamp = rtpTimestamp;
        packet.marker = i + 1 == payloads.size();
    }

    framesQueued = true;
    lastFrameTimestamp = rtpTimestamp;
    lastFrameQueuedUs = nowUs;
    counters.frames++;
    if (keyframe)
        counters.keyframes++;
    return true;
}

void VideoSendStream::retransmit(std::span<const uint16_t> sequences, qint64 nowUs)
{
    counters.nacksReceived += sequences.size();
    if (!retransmits())
        return;

    forgetOld(nowUs);
    for (uint16_t sequence : sequences) {
        if (findSent(sequence) && std::find(resends.begin(), resends.end(), sequence) == resends.end())
            resends.push_back(sequence);
    }
}

void VideoSendStream::clear()
{
    queued.clear();
    resends.clear();
    history.clear();
    intervalAllowance = 0;
}

void VideoSendStream::startInterval()
{
    const size_t waiting = queued.size() + resends.size();
    intervalAllowance = static_cast<int>(std::max<size_t>(PACKETS_PER_INTERVAL, (waiting + DRAIN_INTERVALS - 1) / DRAIN_INTERVALS));
}

bool VideoSendStream::writeNext(qint64 nowUs, PacketBuffer &out)
{
    if (intervalAllowance <= 0)
        return false;
    forgetOld(nowUs);

    while (!resends.empty()) {
        const uint16_t sequence = resends.front();
        resends.pop_front();

        const Packet *original = findSent(sequence);
        if (!original || !write(out, *original, true, nowUs))
            continue;
        intervalAllowance--;
        counters.retransmitted++;
        return true;
    }

    while (!queued.empty()) {
        Packet packet = std::move(queued.front());
        queued.pop_front();
        packet.sequence = nextSequence;
        packet.sentUs = nowUs;
        if (!write(out, packet, false, nowUs))
            continue;

        nextSequence++;
        intervalAllowance--;
        counters.packets++;
        counters.bytes += packet.payload.size();
        history.push_back(std::move(packet));
        return true;
    }
    return false;
}

bool VideoSendStream::write(PacketBuffer &out, const Packet &packet, bool retransmission, qint64 nowUs)
{
    RtpHeader header;
    header.extension = true;
    header.marker = packet.marker;
    header.payloadType = retransmission ? *rtxPayloadType : payloadType;
    header.sequence = retransmission ? nextRtxSequence : packet.sequence;
    header.timestamp = packet.timestamp;
    header.ssrc = retransmission ? retransmissionSsrc : mediaSsrc;

    // abs-send-time is the time in seconds as 6.18 fixed point, cut to its low 24 bits.
    const quint64 sendTime = ((static_cast<quint64>(nowUs) << 18) + 500000) / 1000000;
    const uint8_t sendTimeBytes[3] = { static_cast<uint8_t>(sendTime >> 16), static_cast<uint8_t>(sendTime >> 8), static_cast<uint8_t>(sendTime) };
    // The transmission offset is there but always zero, as the official client sends it.
    const uint8_t noOffset[3] = {};
    uint8_t transportSequence[2];
    qToBigEndian(nextTransportSequence, transportSequence);
    // In the order WebRTC lays them out, which is not the order of their ids.
    const RtpExtension elements[] = { { ABS_SEND_TIME_EXTENSION, sendTimeBytes }, { TRANSMISSION_OFFSET_EXTENSION, noOffset }, { TRANSPORT_SEQUENCE_EXTENSION, transportSequence } };
    uint8_t extensions[EXTENSIONS_SIZE];
    if (writeRtpExtensions(elements, extensions) != EXTENSIONS_SIZE)
        return false;

    out.clear();
    if (writeRtpHeader(header, { out.data(), static_cast<size_t>(out.tailroom()) }, EXTENSIONS_SIZE / 4) != CLEAR_SIZE)
        return false;
    out.resize(CLEAR_SIZE);
    out.append(extensions);
    if (retransmission) {
        uint8_t originalSequence[RTX_HEADER_SIZE];
        qToBigEndian(packet.sequence, originalSequence);
        out.append(originalSequence);
    }
    if (!out.append(packet.payload))
        return false;

    nextTransportSequence++;
    if (retransmission)
        nextRtxSequence++;
    counters.bytesWritten += static_cast<quint64>(out.size());
    return true;
}

const VideoSendStream::Packet *VideoSendStream::findSent(uint16_t sequence) const
{
    if (history.empty())
        return nullptr;
    const uint16_t index = static_cast<uint16_t>(sequence - history.front().sequence);
    return index < history.size() ? &history[index] : nullptr;
}

void VideoSendStream::forgetOld(qint64 nowUs)
{
    while (!history.empty() && nowUs - history.front().sentUs > HISTORY_US)
        history.pop_front();
}

int VideoSendStream::writeSenderReport(std::span<uint8_t> out, qint64 nowUs, uint64_t ntpTime)
{
    if (!canReport())
        return -1;

    SenderInfo info;
    info.ssrc = mediaSsrc;
    info.ntpTime = ntpTime;
    // The RTP time of this moment: the last frame's, moved on by the time since.
    info.rtpTimestamp = lastFrameTimestamp + static_cast<uint32_t>((nowUs - lastFrameQueuedUs) * VIDEO_CLOCK_KHZ / 1000);
    info.packetCount = static_cast<uint32_t>(counters.packets);
    info.octetCount = static_cast<uint32_t>(counters.bytes);

    const int size = Voice::writeSenderReport(out, info);
    if (size < 0)
        return size;

    sentReports.push_back({ compactNtp(ntpTime), nowUs });
    if (sentReports.size() > REMEMBERED_REPORTS)
        sentReports.pop_front();
    return size;
}

void VideoSendStream::onReceptionReport(const ReceptionReport &report, qint64 nowUs)
{
    counters.lossFraction = report.fractionLost / 256.0;

    // Zero stands for no sender report received yet.
    if (report.lastSenderReport == 0)
        return;
    const auto answered = std::find_if(sentReports.begin(), sentReports.end(), [&report](const SentReport &sent) { return sent.compactNtp == report.lastSenderReport; });
    if (answered == sentReports.end())
        return;
    // The receiver says how long it held the report, in units of 1/65536 of a second.
    const qint64 heldUs = static_cast<qint64>(report.delaySinceLastSenderReport) * 1000000 / 65536;
    counters.roundTripMs = static_cast<int>(std::max<qint64>(0, nowUs - answered->sentUs - heldUs) / 1000);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
