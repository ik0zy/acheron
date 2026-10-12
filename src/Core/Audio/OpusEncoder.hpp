#pragma once

#include <QByteArray>

#include <opus.h>

namespace Acheron {
namespace Core {
namespace Audio {

class OpusEncoder
{
public:
    OpusEncoder();
    ~OpusEncoder();

    OpusEncoder(const OpusEncoder &) = delete;
    OpusEncoder &operator=(const OpusEncoder &) = delete;

    bool init(int sampleRate, int channels, int application = OPUS_APPLICATION_VOIP);

    // The most one encoded frame may take, whatever bitrate is asked for. With the RTP header
    // and the end-to-end encryption's trailer added, the packet then stays within the 1200
    // bytes the official client keeps its media packets to. Larger ones are split up by
    // networks with a reduced MTU, a VPN's for one, and split voice packets get lost.
    static constexpr int MAX_PACKET_BYTES = 1160;

    QByteArray encode(const QByteArray &pcmFrame);

    void setBitrate(int bitrate);
    void setComplexity(int complexity);
    void setSignalType(int signalType);
    void setFec(bool enabled);
    void setDtx(bool enabled);
    void setPacketLossPercent(int percent);

private:
    ::OpusEncoder *encoder = nullptr;
    int frameSamples = 0;
    int frameChannels = 0;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
