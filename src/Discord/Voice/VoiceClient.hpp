#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "PacketBuffer.hpp"
#include "RtpPacket.hpp"
#include "SinkWants.hpp"
#include "TransportFeedback.hpp"
#include "VideoReceiver.hpp"
#include "VoiceEnums.hpp"
#include "VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class VoiceGateway;
class UdpTransport;
class VoiceEncryption;
class DaveSession;

// What one connection has received of remote video, summed over its senders.
struct VideoReceiveStats
{
    QString codec;
    quint64 packets = 0;
    quint64 bytes = 0;
    quint64 frames = 0;
    quint64 keyframes = 0;
    quint64 retransmitted = 0;
    quint64 nacked = 0;
    quint64 keyframeRequests = 0;
    quint64 decryptFailures = 0;
    // Share of packets lost since the previous report, 0 to 1.
    double lossFraction = 0;
};

class VoiceClient : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,
        Identifying,
        WaitingForReady,
        DiscoveringIP,
        SelectingProtocol,
        WaitingForSession,
        Connected,
    };
    Q_ENUM(State)

    explicit VoiceClient(const ConnectionParams &params, QObject *parent = nullptr);
    ~VoiceClient() override;

    void start();
    void stop();

    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] quint32 ssrc() const { return localSsrc; }
    [[nodiscard]] const QString &encryptionMode() const { return selectedMode; }
    [[nodiscard]] const QByteArray &secretKey() const { return sessionKey; }

    void seedConnectedUsers(const QList<Core::Snowflake> &userIds);

    void sendAudio(const QByteArray &opusData);

    void setSpeaking(bool speaking);

    bool isDaveEnabled() const;
    void requestVerificationCode(Core::Snowflake targetUserId, std::function<void(const QString &)> callback);

    // Receives every complete, decrypted frame of remote video, on this client's thread. Set it
    // before start().
    using VideoFrameHandler = std::function<void(Core::Snowflake userId, VideoCodec codec, EncodedVideoFrame frame)>;
    void setVideoFrameHandler(VideoFrameHandler handler);

    // How much of the remote video is wanted: the size it is being shown at, in pixels, or
    // nothing at all while it is not shown. This is all a stream's viewer has to say.
    void setVideoWants(int pixelCount, bool wanted);

    // The full form, for a call with several senders: what to tell the server, and which
    // SSRCs to receive and hand on. Everything else that arrives is discarded.
    void setVideoPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver);

    // A frame handed out could not be decoded; ask every sender to start over.
    void requestVideoKeyframe();

    // No decoder here can handle the codec after all: tell the server, so that it has senders
    // use another.
    void withdrawVideoCodec(VideoCodec codec);

signals:
    void stateChanged(State newState);
    void connected();
    void disconnected();
    // The gateway socket dropped and is being reopened.
    void reconnecting();

    // A user's video started, changed or (inactive) stopped. The size and rate are the most the
    // sender says it will send, and are zero when it does not say.
    void remoteVideoChanged(Acheron::Core::Snowflake userId, bool active, int maxWidth, int maxHeight, int maxFramerate);
    // The encodings a user's video now comes in; none when it has stopped.
    void remoteVideoLayersChanged(Acheron::Core::Snowflake userId, const QList<Acheron::Discord::Voice::VideoLayer> &layers);
    void videoStatsUpdated(const Acheron::Discord::Voice::VideoReceiveStats &stats);

    void speakingReceived(const SpeakingData &data);
    void clientConnected(const ClientConnectData &data);
    void clientDisconnected(Core::Snowflake userId);

    void audioReceived(quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData);

    void privacyCodeChanged(const QString &code);

private slots:
    void onGatewayConnected();
    void onGatewayDisconnected(VoiceCloseCode code, const QString &reason);
    void onGatewayReady(const VoiceReady &data);
    void onSessionDescription(const SessionDescription &desc);
    void onSpeaking(const SpeakingData &data);
    void onClientsConnect(const QStringList &userIds);
    void onClientConnect(const ClientConnectData &data);
    void onClientDisconnect(Core::Snowflake userId);
    void onGatewayResumed();
    void onIpDiscovered(const QString &ip, int port);
    void onIpDiscoveryFailed(const QString &error);
    void onRelayLost(const QString &error);

private:
    void setState(State state);
    void sendSilence();
    void cleanupTransport();
    void ensureDaveSession(uint16_t protocolVersion);

    void onPacket(PacketPool::Handle packet);
    void receiveAudio(const ParsedRtp &rtp, PacketPool::Handle packet);
    // Replaces the packet with its end-to-end decrypted contents.
    bool daveDecryptAudio(quint32 ssrc, PacketPool::Handle &packet);

    struct RemoteVideo
    {
        Core::Snowflake userId;
        quint32 rtxSsrc = 0;
        std::unique_ptr<VideoReceiver> receiver;
    };

    void receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, bool retransmission);
    void deliverVideoFrames(quint32 ssrc, RemoteVideo &video);
    bool daveDecryptVideo(quint32 ssrc, std::vector<uint8_t> &frame);
    void applyRemoteVideo(const ClientConnectData &data);
    void removeRemoteVideo(Core::Snowflake userId);
    void planSingleSenders();
    void applyVideoPlan();
    void startVideoTimer();
    void onVideoTick();
    void sendVideoWants();

private:
    VoiceGateway *gateway = nullptr;
    UdpTransport *udpTransport = nullptr;

    ConnectionParams params;

    std::atomic<State> currentState{ State::Disconnected };

    // Packets this connection receives but has no consumer for yet.
    struct UnroutedCounters
    {
        quint64 rtcp = 0;
        quint64 rtp = 0;
    };
    UnroutedCounters unrouted;

    // ready
    quint32 localSsrc = 0;
    QString serverIp;
    int serverPort = 0;
    QStringList serverModes;

    // protocol selection
    QString selectedMode;
    QByteArray sessionKey;

    std::unique_ptr<VoiceEncryption> encryption;
    QTimer *keepaliveTimer = nullptr;
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;

    std::chrono::steady_clock::time_point rtpEpoch;
    std::chrono::steady_clock::time_point lastAudioSendTime;
    bool newTalkspurt = false;

    std::unique_ptr<DaveSession> daveSession;
    std::set<std::string> connectedUserIds;
    QHash<quint32, uint64_t> ssrcToUserIdMap;

    // The codec behind each payload type this client offered, for video and for its RTX.
    QHash<int, VideoCodec> videoPayloadTypes;
    QHash<int, VideoCodec> rtxPayloadTypes;
    // Remote video by its SSRC, and each RTX SSRC's primary.
    std::unordered_map<quint32, RemoteVideo> remoteVideos;
    QHash<quint32, quint32> rtxToPrimary;
    QHash<Core::Snowflake, QList<VideoLayer>> remoteLayers;
    // What the server is asked for, and the SSRCs whose video is received and handed on.
    MediaSinkWants plannedWants;
    QSet<quint32> deliveredSsrcs;
    bool explicitVideoPlan = false;
    VideoFrameHandler videoFrameHandler;
    QTimer *videoTimer = nullptr;
    QElapsedTimer videoClock;
    qint64 videoReportedMs = 0;
    int wantedPixelCount = 0;
    bool videoWanted = true;
    QByteArray sentVideoWants;
    quint64 videoDecryptFailures = 0;

    // Which packets arrived and when, for the server's estimate of what this connection can
    // carry. Without it the server holds every stream at a low bitrate.
    TransportFeedback transportFeedback;
    qint64 transportReportedMs = 0;
    quint32 transportMediaSsrc = 0;

    static constexpr int KEEPALIVE_INTERVAL_MS = 10000;
    static constexpr int VIDEO_TICK_MS = 20;
    static constexpr int VIDEO_REPORT_INTERVAL_MS = 1000;
    static constexpr int TRANSPORT_FEEDBACK_INTERVAL_MS = 100;
    // The header extension that carries a packet's transport-wide sequence number.
    static constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Discord::Voice::VideoReceiveStats)
