#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"
#include "Rtcp.hpp"
#include "VideoSender.hpp"

#include "DaveSession.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <QDateTime>
#include <QRandomGenerator>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <utility>

namespace Acheron {
namespace Discord {
namespace Voice {

// 20ms at 48khz
static constexpr uint32_t OPUS_FRAME_SAMPLES = 960;

static QString formatDisplayableCode(const std::vector<uint8_t> &data, int bytesToConsume = 30, int groupSize = 5)
{
    int numGroups = bytesToConsume / groupSize;
    if (static_cast<int>(data.size()) < bytesToConsume)
        return {};

    uint64_t modulus = 1;
    for (int i = 0; i < groupSize; i++)
        modulus *= 10;

    QString result;
    for (int g = 0; g < numGroups; g++) {
        uint64_t value = 0;
        for (int b = 0; b < groupSize; b++)
            value = (value << 8) | data[g * groupSize + b];
        value %= modulus;

        if (g > 0)
            result += QLatin1Char(' ');
        result += QStringLiteral("%1").arg(value, groupSize, 10, QLatin1Char('0'));
    }
    return result;
}

// The wall clock as a sender report states it: seconds since 1900 over a binary fraction.
static uint64_t ntpTimeNow()
{
    constexpr quint64 SECONDS_FROM_1900_TO_1970 = 2208988800ULL;
    const quint64 ms = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    return ((ms / 1000 + SECONDS_FROM_1900_TO_1970) << 32) | (((ms % 1000) << 32) / 1000);
}

QList<StreamDescription> cameraStreams()
{
    StreamDescription layer;
    layer.type = QStringLiteral("video");
    layer.rid = QStringLiteral("100");
    layer.quality = 100;
    return { layer };
}

VoiceClient::VoiceClient(const ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params)
{
    qRegisterMetaType<VideoReceiveStats>();
    qRegisterMetaType<VideoSendStats>();
    qRegisterMetaType<VideoCodec>();
    qRegisterMetaType<QList<VideoLayer>>();

    for (const Codec &codec : params.videoCodecs) {
        const VideoCodec known = videoCodecFromName(codec.name);
        if (known == VideoCodec::Unknown || !codec.decode.valueOr(false))
            continue;
        videoPayloadTypes.insert(codec.payloadType, known);
        if (codec.rtxPayloadType.hasValue())
            rtxPayloadTypes.insert(codec.rtxPayloadType.get(), known);
    }
}

void VoiceClient::setVideoFrameHandler(VideoFrameHandler handler)
{
    videoFrameHandler = std::move(handler);
}

VoiceClient::~VoiceClient()
{
    stop();
}

void VoiceClient::seedConnectedUsers(const QList<Core::Snowflake> &userIds)
{
    for (auto id : userIds)
        connectedUserIds.insert(std::to_string(id));
}

void VoiceClient::start()
{
    if (currentState != State::Disconnected) {
        qCWarning(LogVoice) << "VoiceClient::start called in non-disconnected state";
        return;
    }

    VoiceEncryption::initialize();

    setState(State::Connecting);

    gateway = new VoiceGateway(params, this);

    connect(gateway, &VoiceGateway::connected, this, &VoiceClient::onGatewayConnected);
    connect(gateway, &VoiceGateway::disconnected, this, &VoiceClient::onGatewayDisconnected);
    connect(gateway, &VoiceGateway::readyReceived, this, &VoiceClient::onGatewayReady);
    connect(gateway, &VoiceGateway::sessionDescriptionReceived, this, &VoiceClient::onSessionDescription);
    connect(gateway, &VoiceGateway::sessionUpdated, this, &VoiceClient::onSessionUpdated);
    connect(gateway, &VoiceGateway::mediaSinkWantsReceived, this, &VoiceClient::onMediaSinkWants);
    connect(gateway, &VoiceGateway::speakingReceived, this, &VoiceClient::onSpeaking);
    connect(gateway, &VoiceGateway::clientConnected, this, &VoiceClient::onClientConnect);
    connect(gateway, &VoiceGateway::clientsConnected, this, &VoiceClient::onClientsConnect);
    connect(gateway, &VoiceGateway::clientDisconnected, this, &VoiceClient::onClientDisconnect);
    connect(gateway, &VoiceGateway::resumed, this, &VoiceClient::onGatewayResumed);
    connect(gateway, &VoiceGateway::reconnecting, this, &VoiceClient::reconnecting);

    connect(gateway, &VoiceGateway::binaryPayloadReceived,
            this, [this](int opcode, const QByteArray &payload) {
                if (!daveSession)
                    return;
                switch (opcode) {
                case static_cast<int>(VoiceOpCode::DAVE_MLS_EXTERNAL_SENDER_PACKAGE):
                    daveSession->onExternalSenderPackage(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_PROPOSALS):
                    daveSession->onProposals(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_ANNOUNCE_COMMIT_TRANSITION):
                    daveSession->onAnnounceCommitTransition(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_WELCOME):
                    daveSession->onWelcome(payload);
                    break;
                default:
                    qCDebug(LogVoice) << "Unhandled DAVE binary opcode:" << opcode;
                    break;
                }
            });
    connect(gateway, &VoiceGateway::daveTransitionPrepare,
            this, [this](int protocolVersion, int transitionId) {
                if (daveSession)
                    daveSession->onPrepareTransition(protocolVersion, transitionId);
            });
    connect(gateway, &VoiceGateway::daveTransitionExecute,
            this, [this](int transitionId) {
                if (daveSession)
                    daveSession->onExecuteTransition(transitionId);
            });
    connect(gateway, &VoiceGateway::daveEpochPrepare,
            this, [this](int protocolVersion, int epoch) {
                if (!daveSession && protocolVersion > 0) {
                    // mid-call upgrade attempt. return cuz onPrepareEpoch also reinits
                    ensureDaveSession(static_cast<uint16_t>(protocolVersion));
                    return;
                }
                if (daveSession)
                    daveSession->onPrepareEpoch(protocolVersion, epoch);
            });

    gateway->start();
}

void VoiceClient::stop()
{
    if (currentState == State::Disconnected)
        return;

    if (gateway) {
        gateway->hardStop();
        delete gateway;
        gateway = nullptr;
    }

    cleanupTransport();

    if (unrouted.rtcp != 0 || unrouted.rtp != 0)
        qCDebug(LogVoice) << "Packets received with no consumer: RTCP" << unrouted.rtcp << "RTP" << unrouted.rtp;
    unrouted = {};

    localSsrc = 0;
    localVideoStreams.clear();
    sendCodec = VideoCodec::Unknown;
    selectedMode.clear();
    sessionKey.clear();

    connectedUserIds.clear();
    ssrcToUserIdMap.clear();

    setState(State::Disconnected);
}

void VoiceClient::cleanupTransport()
{
    if (keepaliveTimer) {
        keepaliveTimer->stop();
        delete keepaliveTimer;
        keepaliveTimer = nullptr;
    }

    if (videoTimer) {
        videoTimer->stop();
        delete videoTimer;
        videoTimer = nullptr;
    }
    // The receivers hold packets from the transport's pool.
    remoteVideos.clear();
    rtxToPrimary.clear();
    remoteLayers.clear();
    deliveredSsrcs.clear();
    sentVideoWants.clear();
    transportFeedback = TransportFeedback();
    senderReports.clear();
    serverEstimateBps = 0;
    serverLossFraction = 0;

    delete videoPaceTimer;
    videoPaceTimer = nullptr;
    delete videoSendReportTimer;
    videoSendReportTimer = nullptr;
    delete localKeyframeTimer;
    localKeyframeTimer = nullptr;
    videoSender.reset();
    localVideo.reset();
    videoBitrate = 0;
    videoReceiverEstimateBps = 0;
    announcedVideoStreams.clear();
    localKeyframeAskedMs = -1;
    localKeyframeRequests = 0;
    videoSendAwaitsKeyframe = false;
    videoReceivers = -1;

    encryption.reset();

    daveSession.reset();

    delete udpTransport;
    udpTransport = nullptr;

    rtpSequence = 0;
    rtpTimestamp = 0;
    rtpEpoch = {};
    soundshareAnnounced = false;
}

void VoiceClient::onGatewayConnected()
{
    qCInfo(LogVoice) << "Voice gateway WebSocket connected, waiting for Hello + Identify";
    setState(State::Identifying);
}

void VoiceClient::onGatewayDisconnected(VoiceCloseCode code, const QString &reason)
{
    qCWarning(LogVoice) << "Voice gateway disconnected, code:" << code << "reason:" << reason;

    cleanupTransport();

    // done if not reconnected
    if (currentState != State::Disconnected) {
        setState(State::Disconnected);
        emit disconnected();
    }
}

void VoiceClient::onGatewayReady(const VoiceReady &data)
{
    qCInfo(LogVoice) << "Voice Ready: SSRC =" << data.ssrc
                     << "server =" << data.ip << ":" << data.port
                     << "modes =" << data.modes.get();

    localSsrc = data.ssrc;
    takeLocalVideoStreams(data);
    serverIp = data.ip;
    serverPort = data.port;
    serverModes = data.modes;

    static const std::array preferred = {
        EncryptionMode::AEAD_AES256_GCM_RTPSIZE,
        EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE,
    };

    EncryptionMode mode = EncryptionMode::UNKNOWN;
    for (auto candidate : preferred) {
        if (serverModes.contains(encryptionModeToString(candidate)) && VoiceEncryption::isModeAvailable(candidate)) {
            mode = candidate;
            break;
        }
    }

    if (mode == EncryptionMode::UNKNOWN) {
        qCCritical(LogVoice) << "No supported encryption mode found! Server offered:" << serverModes;
        stop();
        return;
    }
    selectedMode = encryptionModeToString(mode);
    qCInfo(LogVoice) << "Selected encryption mode:" << selectedMode;

    setState(State::DiscoveringIP);

    if (params.video)
        gateway->sendVoiceBackendVersion();

    cleanupTransport();

    udpTransport = new UdpTransport(params.proxy, this);
    connect(udpTransport, &UdpTransport::ipDiscovered, this, &VoiceClient::onIpDiscovered);
    connect(udpTransport, &UdpTransport::ipDiscoveryFailed, this, &VoiceClient::onIpDiscoveryFailed);
    connect(udpTransport, &UdpTransport::relayLost, this, &VoiceClient::onRelayLost);
    udpTransport->setPacketHandler([this](PacketPool::Handle packet) { onPacket(std::move(packet)); });

    udpTransport->startIpDiscovery(serverIp, serverPort, localSsrc);
}

void VoiceClient::onSessionDescription(const SessionDescription &desc)
{
    qCInfo(LogVoice) << "Session established: mode =" << desc.mode
                     << "key length =" << desc.secretKey->size();

    sessionKey = desc.secretKey;
    selectedMode = desc.mode;

    EncryptionMode mode = encryptionModeFromString(selectedMode);
    encryption = std::make_unique<VoiceEncryption>(mode, sessionKey);

    int daveVersion = desc.daveProtocolVersion.hasValue() ? desc.daveProtocolVersion.get() : 0;
    qCInfo(LogVoice) << "dave_protocol_version =" << daveVersion;

    if (daveVersion > 0)
        ensureDaveSession(static_cast<uint16_t>(daveVersion));

    rtpEpoch = std::chrono::steady_clock::now();

    // Whoever starts video from connected() has to find the codec already known.
    setVideoSendCodec(desc.videoCodec.valueOr(QString()));

    setState(State::Connected);
    emit connected();

    if (std::exchange(codecsChangedSinceOffered, false))
        sendOfferedCodecs();

    if (!videoPayloadTypes.isEmpty())
        startVideoTimer();
    sendVideoWants();

    // send silence so discord sends us audio immediately
    sendSilence();

    if (!keepaliveTimer) {
        keepaliveTimer = new QTimer(this);
        connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
    }
    keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
}

void VoiceClient::sendAudio(const QByteArray &opusData)
{
    if (currentState != State::Connected || !encryption || !udpTransport || !params.sendsAudio)
        return;

    // snap rtp timestamp back to wall clock after a period of silence
    // otherwise its a little behind and it will be played back delayed by discord
    auto now = std::chrono::steady_clock::now();
    if (newTalkspurt) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - rtpEpoch);
        rtpTimestamp = static_cast<uint32_t>(static_cast<uint64_t>(elapsed.count()) * 48 / 1000);
    } else {
        rtpTimestamp += OPUS_FRAME_SAMPLES;
    }

    RtpHeader header;
    header.payloadType = Codec::OPUS_PAYLOAD_TYPE;
    header.marker = newTalkspurt;
    header.sequence = rtpSequence++;
    header.timestamp = rtpTimestamp;
    header.ssrc = localSsrc;

    newTalkspurt = false;

    PacketPool::Handle packet = udpTransport->acquirePacket();
    const int headerSize = writeRtpHeader(header, { packet->data(), static_cast<size_t>(packet->tailroom()) });
    packet->resize(headerSize);

    const std::span<const uint8_t> opus(reinterpret_cast<const uint8_t *>(opusData.constData()), static_cast<size_t>(opusData.size()));

    if (isDaveEnabled()) {
        auto *enc = daveSession->encryptor();
        const size_t maxSize = enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, opus.size());
        if (static_cast<int>(maxSize) > packet->tailroom() - VoiceEncryption::TRAILER_SIZE)
            return;

        size_t bytesWritten = 0;
        auto result = enc->Encrypt(discord::dave::MediaType::Audio, localSsrc, discord::dave::ArrayView<const uint8_t>(opus.data(), opus.size()), discord::dave::ArrayView<uint8_t>(packet->data() + headerSize, maxSize), &bytesWritten);
        if (result != discord::dave::IEncryptor::Success) {
            qCWarning(LogVoice) << "DAVE encrypt failed: result =" << static_cast<int>(result);
            return;
        }
        packet->resize(headerSize + static_cast<int>(bytesWritten));
    } else if (!packet->append(opus)) {
        return;
    }

    if (!encryption->seal(*packet, headerSize))
        return;

    udpTransport->send(*packet);

    lastAudioSendTime = now;
}

void VoiceClient::setSpeaking(bool speaking)
{
    if (!gateway)
        return;

    if (speaking)
        newTalkspurt = true;

    int flags = speaking ? static_cast<int>(SpeakingFlag::MICROPHONE) : 0;
    gateway->sendSpeaking(flags, 0, localSsrc);
}

void VoiceClient::beginSoundshare()
{
    if (currentState != State::Connected || !gateway || !params.sendsAudio)
        return;

    newTalkspurt = true;
    if (std::exchange(soundshareAnnounced, true))
        return;
    gateway->sendSpeaking(static_cast<int>(SpeakingFlag::SOUNDSHARE), 0, localSsrc);
}

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // The silence is the microphone's, and a stream's connection has no microphone: its owner
    // sends the sound it shares and nothing in between.
    if (params.streamContext)
        return;

    // no need to keepalive by sending silence if we spoke recently
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioSendTime);
    if (elapsed.count() < 5)
        return;

    QByteArray silencePayload(reinterpret_cast<const char *>(Core::Audio::OPUS_SILENCE), sizeof(Core::Audio::OPUS_SILENCE));
    sendAudio(silencePayload);

    qCDebug(LogVoice) << "Sent keepalive silence frame";
}

void VoiceClient::onPacket(PacketPool::Handle packet)
{
    switch (classifyPacket(packet->bytes())) {
    case PacketKind::Rtcp:
        receiveRtcp(std::move(packet));
        return;
    case PacketKind::Unknown:
        return;
    case PacketKind::Rtp:
        break;
    }

    const std::optional<ParsedRtp> rtp = parseRtp(packet->bytes());
    if (!rtp)
        return;

    const int payloadType = rtp->header.payloadType;
    if (payloadType == Codec::OPUS_PAYLOAD_TYPE) {
        receiveAudio(*rtp, std::move(packet));
        return;
    }

    if (auto video = videoPayloadTypes.constFind(payloadType); video != videoPayloadTypes.constEnd()) {
        receiveVideo(*rtp, std::move(packet), video.value(), false);
        return;
    }
    if (auto rtx = rtxPayloadTypes.constFind(payloadType); rtx != rtxPayloadTypes.constEnd()) {
        receiveVideo(*rtp, std::move(packet), rtx.value(), true);
        return;
    }

    // Nothing this client plays, but the server numbers these too: it tries the link with
    // padding under a payload type of its own, and a packet not reported counts as lost.
    if (encryption && encryption->open(*packet, rtp->clearSize)) {
        packet->dropFront(rtp->clearSize);
        if (packet->size() >= rtp->extensionSize)
            noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp->extensionSize)));
    }
    unrouted.rtp++;
}

void VoiceClient::noteTransportSequence(std::span<const uint8_t> extensions)
{
    // Only while feedback is going out; otherwise the record would grow without end.
    if (!videoTimer || !videoTimer->isActive())
        return;
    const std::span<const uint8_t> sequence = findRtpExtension(extensions, TRANSPORT_SEQUENCE_EXTENSION);
    if (sequence.size() == 2)
        transportFeedback.onPacket(qFromBigEndian<uint16_t>(sequence.data()), videoClock.nsecsElapsed() / 1000);
}

void VoiceClient::receiveRtcp(PacketPool::Handle packet)
{
    if (!encryption || !encryption->open(*packet, RTCP_CLEAR_SIZE)) {
        unrouted.rtcp++;
        return;
    }

    const ReceivedRtcp received = parseRtcp(packet->bytes());
    receiveVideoFeedback(received);
    for (uint32_t ssrc : received.videoResets)
        receiveVideoAfresh(ssrc);
    if (videoClock.isValid()) {
        const qint64 now = videoClock.nsecsElapsed() / 1000;
        for (const ReceivedRtcp::SenderReport &report : received.senderReports)
            senderReports.insert(report.ssrc, { report.compactNtp, now });
    }
    for (const ReceptionReport &report : received.receptionReports) {
        // Whether what this client sends is arriving is otherwise only known to the people
        // who cannot hear it.
        if (report.ssrc == localSsrc)
            qCDebug(LogVoice) << "Server's report on our audio: has up to sequence" << (report.highestSequence & 0xFFFF) << "of" << static_cast<uint16_t>(rtpSequence - 1) << "sent, lost in all" << report.cumulativeLost << "lost lately" << report.fractionLost / 256.0;
    }
    if (received.hasInboundEstimate) {
        if (received.inboundBitsPerSecond != serverEstimateBps)
            qCDebug(LogVoice) << "Server's estimate of the way here:" << received.inboundBitsPerSecond << "bps, loss" << received.inboundLoss;
        serverEstimateBps = received.inboundBitsPerSecond;
        serverLossFraction = received.inboundLoss;
    }
}

void VoiceClient::receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, bool retransmission)
{
    const RtpHeader &header = rtp.header;
    if (!encryption || !encryption->open(*packet, rtp.clearSize))
        return;

    packet->dropFront(rtp.clearSize);
    if (packet->size() < rtp.extensionSize)
        return;
    // Every packet the server numbered is reported back to it, wanted or not, since the
    // numbering is about the connection and not about any one stream.
    noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp.extensionSize)));
    transportMediaSsrc = header.ssrc;
    packet->dropFront(rtp.extensionSize);

    if (header.padding) {
        const int padding = rtpPaddingSize(packet->bytes());
        if (padding < 0)
            return;
        packet->resize(packet->size() - padding);
    }

    quint32 ssrc = header.ssrc;
    uint16_t sequence = header.sequence;
    if (retransmission) {
        // RFC 4588: the original sequence number leads the payload. One without a payload is
        // padding sent to probe the link.
        auto primary = rtxToPrimary.constFind(ssrc);
        if (primary == rtxToPrimary.constEnd() || packet->size() <= 2)
            return;
        ssrc = primary.value();
        sequence = qFromBigEndian<uint16_t>(packet->data());
        packet->dropFront(2);
    }

    auto it = remoteVideos.find(ssrc);
    if (it == remoteVideos.end() || packet->size() == 0) {
        unrouted.rtp++;
        return;
    }
    if (!deliveredSsrcs.contains(ssrc))
        return;

    RemoteVideo &video = it->second;
    if (!video.receiver || video.receiver->codec() != codec)
        video.receiver = std::make_unique<VideoReceiver>(ssrc, codec);

    video.receiver->insert(sequence, header.timestamp, header.marker, std::move(packet), videoClock.elapsed(), retransmission);
    deliverVideoFrames(ssrc, video);
}

void VoiceClient::deliverVideoFrames(quint32 ssrc, RemoteVideo &video)
{
    std::vector<EncodedVideoFrame> frames = video.receiver->takeFrames();
    const VideoCodec codec = video.receiver->codec();

    for (EncodedVideoFrame &frame : frames) {
        if (daveSession) {
            if (daveSession->isDaveEnabled()) {
                if (!daveDecryptVideo(ssrc, frame.data)) {
                    // Later frames build on this one, so they are of no use either.
                    videoDecryptFailures++;
                    qCDebug(LogDave) << "Undecryptable" << describeFrame(codec, frame.data);
                    video.receiver->requestKeyframe();
                    return;
                }
            } else if (!daveSession->isDowngraded()) {
                // Keys are still being agreed. A key frame is asked for once they are.
                videoDroppedAwaitingDaveKeys = true;
                return;
            }
        }

        prepareForDecoder(codec, frame.data);
        if (videoFrameHandler)
            videoFrameHandler(video.userId, codec, std::move(frame));
    }
}

bool VoiceClient::daveDecryptVideo(quint32 ssrc, std::vector<uint8_t> &frame)
{
    auto *decryptor = daveSession->getOrCreateDecryptor(ssrc, ssrcToUserIdMap.value(ssrc, 0));

    std::vector<uint8_t> plaintext(decryptor->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, frame.size()));
    size_t bytesWritten = 0;
    const auto result = decryptor->Decrypt(discord::dave::MediaType::Video, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(plaintext.data(), plaintext.size()), &bytesWritten);
    if (result != discord::dave::IDecryptor::Success) {
        qCDebug(LogDave) << "DAVE video decrypt failed: SSRC =" << ssrc << "result =" << static_cast<int>(result) << "frameSize =" << frame.size();
        return false;
    }

    plaintext.resize(bytesWritten);
    frame.swap(plaintext);
    return true;
}

void VoiceClient::applyRemoteVideo(const ClientConnectData &data)
{
    if (videoPayloadTypes.isEmpty() || !data.userId.hasValue())
        return;
    const Core::Snowflake userId = data.userId.get();
    if (!userId.isValid() || userId == params.userId)
        return;

    // A user's video is on while the announcement names a video SSRC; each entry of `streams`
    // is then one encoding of it. An announcement with no entries means a single encoding.
    QList<VideoLayer> layers;
    QList<VideoLayer> flaggedInactive;
    int maxWidth = 0;
    int maxHeight = 0;
    int maxFramerate = 0;
    if (data.videoSsrc.get() != 0) {
        if (data.streams.hasValue()) {
            for (const StreamDescription &stream : data.streams.get()) {
                if (!stream.ssrc.hasValue() || stream.ssrc.get() == 0)
                    continue;
                VideoLayer layer;
                layer.ssrc = stream.ssrc.get();
                layer.rtxSsrc = stream.rtxSsrc.hasValue() && stream.rtxSsrc.get() != 0 ? stream.rtxSsrc.get() : layer.ssrc + 1;
                layer.quality = stream.quality.valueOr(100);
                if (!stream.active.valueOr(true)) {
                    flaggedInactive.append(layer);
                    continue;
                }
                layers.append(layer);

                if (layer.quality == 100 || maxWidth == 0) {
                    if (stream.maxResolution.hasValue()) {
                        maxWidth = stream.maxResolution->width;
                        maxHeight = stream.maxResolution->height;
                    }
                    maxFramerate = stream.maxFramerate.valueOr(0);
                }
            }
        }
        // On a voice connection the official client goes by the video SSRC alone and ignores
        // each entry's own flag, so entries all flagged inactive still count there.
        if (layers.isEmpty() && !params.streamContext)
            layers = flaggedInactive;
        if (layers.isEmpty() && (!data.streams.hasValue() || data.streams->isEmpty())) {
            VideoLayer layer;
            layer.ssrc = data.videoSsrc;
            layer.rtxSsrc = layer.ssrc + 1;
            layers.append(layer);
        }
    }

    if (remoteLayers.value(userId) != layers) {
        removeRemoteVideo(userId);
        for (const VideoLayer &layer : layers) {
            RemoteVideo video;
            video.userId = userId;
            video.rtxSsrc = layer.rtxSsrc;
            remoteVideos.emplace(layer.ssrc, std::move(video));
            rtxToPrimary.insert(layer.rtxSsrc, layer.ssrc);

            ssrcToUserIdMap.insert(layer.ssrc, userId);
            if (daveSession)
                daveSession->applyKeyRatchetForSsrc(layer.ssrc, userId);
        }
        if (!layers.isEmpty())
            remoteLayers.insert(userId, layers);
    }

    // On a voice connection whoever shows the video decides what is wanted, and is told here.
    if (!explicitVideoPlan && params.streamContext)
        planSingleSenders();
    emit remoteVideoLayersChanged(userId, layers);
    emit remoteVideoChanged(userId, !layers.isEmpty(), maxWidth, maxHeight, maxFramerate);
}

void VoiceClient::removeRemoteVideo(Core::Snowflake userId)
{
    for (auto it = remoteVideos.begin(); it != remoteVideos.end();) {
        if (it->second.userId != userId) {
            ++it;
            continue;
        }
        rtxToPrimary.remove(it->second.rtxSsrc);
        ssrcToUserIdMap.remove(it->first);
        deliveredSsrcs.remove(it->first);
        it = remoteVideos.erase(it);
    }
    remoteLayers.remove(userId);
}

void VoiceClient::setVideoWants(int pixelCount, bool wanted)
{
    explicitVideoPlan = false;
    wantedPixelCount = pixelCount;
    videoWanted = wanted;
    planSingleSenders();
}

// The plan for a stream connection, where the only sender is the broadcaster: its first
// encoding at full quality, sized to the surface, or nothing while the surface is not shown.
void VoiceClient::planSingleSenders()
{
    plannedWants = MediaSinkWants();
    deliveredSsrcs.clear();
    for (auto it = remoteLayers.constBegin(); it != remoteLayers.constEnd(); ++it) {
        const quint32 ssrc = it.value().first().ssrc;
        plannedWants.bySsrc.insert(ssrc, videoWanted ? 100 : 0);
        if (videoWanted) {
            plannedWants.pixelCounts.insert(ssrc, wantedPixelCount);
            deliveredSsrcs.insert(ssrc);
        }
    }
    applyVideoPlan();
}

void VoiceClient::setVideoPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver)
{
    explicitVideoPlan = true;
    plannedWants = wants;
    deliveredSsrcs = deliver;
    applyVideoPlan();
}

void VoiceClient::applyVideoPlan()
{
    // An encoding no longer taken starts from a key frame if it is taken again.
    for (auto &[ssrc, video] : remoteVideos) {
        if (!deliveredSsrcs.contains(ssrc))
            video.receiver.reset();
    }
    sendVideoWants();
}

VideoCodec VoiceClient::videoSendCodec() const
{
    return sendCodec;
}

void VoiceClient::takeLocalVideoStreams(const VoiceReady &ready)
{
    localVideoStreams = ready.streams.valueOr();
    if (localVideoStreams.isEmpty()) {
        // Given none, the official client takes the two SSRCs that follow its audio one.
        StreamDescription stream;
        stream.type = QStringLiteral("video");
        stream.rid = QStringLiteral("100");
        stream.ssrc = localSsrc + 1;
        stream.quality = 100;
        stream.rtxSsrc = localSsrc + 2;
        localVideoStreams.append(stream);
    }

    // The official client's limits for what it captures go to the full-quality layer.
    localVideoLayer = 0;
    for (int i = 0; i < localVideoStreams.size(); i++) {
        if (localVideoStreams[i].quality.valueOr(0) == 100) {
            localVideoLayer = i;
            break;
        }
    }
}

quint32 VoiceClient::localVideoSsrc() const
{
    return localVideoLayer < localVideoStreams.size() ? localVideoStreams[localVideoLayer].ssrc.valueOr(0) : 0;
}

void VoiceClient::offerVideoEncoding(const QList<VideoCodec> &encodable)
{
    bool changed = false;
    for (Codec &offered : params.videoCodecs) {
        const bool encoded = encodable.contains(videoCodecFromName(offered.name));
        if (offered.encode.valueOr(false) == encoded)
            continue;
        offered.encode = encoded;
        changed = true;
    }
    if (!changed)
        return;

    if (currentState == State::WaitingForSession)
        codecsChangedSinceOffered = true;
    if (currentState != State::Connected)
        return;

    sendOfferedCodecs();
    // The server may have named this codec already, when it could not be taken up.
    setVideoSendCodec(serverVideoCodec);
}

// A video codec that is neither encoded nor decoded any more is left out, as the official
// client leaves it out.
QList<Codec> VoiceClient::offeredCodecs() const
{
    QList<Codec> codecs = { Codec::opus() };
    for (const Codec &video : params.videoCodecs) {
        if (video.encode.valueOr(false) || video.decode.valueOr(false))
            codecs.append(video);
    }
    return codecs;
}

void VoiceClient::sendOfferedCodecs()
{
    gateway->sendSessionUpdate(offeredCodecs());
}

void VoiceClient::setVideoSendCodec(const QString &name)
{
    serverVideoCodec = name;

    // The server picks among the codecs this client said it encodes.
    VideoCodec codec = VideoCodec::Unknown;
    for (const Codec &offered : params.videoCodecs) {
        if (offered.name.get() == name && offered.encode.valueOr(false))
            codec = videoCodecFromName(name);
    }
    if (codec == sendCodec)
        return;

    qCInfo(LogVoice) << "Video is to be sent as" << (codec == VideoCodec::Unknown ? QStringLiteral("nothing this client encodes") : name);
    sendCodec = codec;
    if (localVideo) {
        prepareVideoSender();
        // Frames of the old codec may still be on their way here.
        videoSendAwaitsKeyframe = true;
    }
    emit videoSendCodecChanged(codec);
}

bool VoiceClient::prepareVideoSender()
{
    const quint32 ssrc = localVideoSsrc();
    const auto offered = std::find_if(params.videoCodecs.cbegin(), params.videoCodecs.cend(), [this](const Codec &codec) { return videoCodecFromName(codec.name) == sendCodec && codec.encode.valueOr(false); });
    if (ssrc == 0 || offered == params.videoCodecs.cend() || !VideoPacketizer::supports(sendCodec)) {
        if (videoSender)
            videoSender->setCodec(VideoCodec::Unknown, 0, std::nullopt);
        return false;
    }

    if (!videoSender) {
        const StreamDescription &layer = localVideoStreams[localVideoLayer];
        const quint32 rtxSsrc = layer.rtxSsrc.hasValue() && layer.rtxSsrc.get() != 0 ? layer.rtxSsrc.get() : ssrc + 1;

        // As WebRTC starts them: the sequence numbers and the picture id anywhere in the lower
        // half of their range, the transport-wide numbering at one.
        QRandomGenerator *random = QRandomGenerator::global();
        VideoSendStream::Numbering first;
        first.sequence = static_cast<uint16_t>(random->bounded(1, 0x8000));
        first.rtxSequence = static_cast<uint16_t>(random->bounded(1, 0x8000));
        first.pictureId = static_cast<uint16_t>(random->bounded(0x8000));
        first.transportSequence = 1;
        videoSender = std::make_unique<VideoSendStream>(ssrc, rtxSsrc, first);
    }

    std::optional<uint8_t> rtxPayloadType;
    if (offered->rtxPayloadType.hasValue())
        rtxPayloadType = static_cast<uint8_t>(offered->rtxPayloadType.get());
    videoSender->setCodec(sendCodec, static_cast<uint8_t>(offered->payloadType.get()), rtxPayloadType);
    paceVideoByBitrate();
    registerLocalVideoWithDave();
    return true;
}

void VoiceClient::setVideoBitrate(int bitsPerSecond)
{
    videoBitrate = bitsPerSecond;
    paceVideoByBitrate();
}

void VoiceClient::paceVideoByBitrate()
{
    if (videoSender && localVideo)
        videoSender->setBitrate(videoBitrate > 0 ? videoBitrate : localVideo->maxBitrate);
}

void VoiceClient::registerLocalVideoWithDave()
{
    if (!daveSession || !videoSender)
        return;

    switch (videoSender->codec()) {
    case VideoCodec::H264:
        daveSession->setLocalVideoCodec(videoSender->ssrc(), discord::dave::Codec::H264);
        break;
    case VideoCodec::H265:
        daveSession->setLocalVideoCodec(videoSender->ssrc(), discord::dave::Codec::H265);
        break;
    case VideoCodec::VP8:
        daveSession->setLocalVideoCodec(videoSender->ssrc(), discord::dave::Codec::VP8);
        break;
    case VideoCodec::AV1:
        daveSession->setLocalVideoCodec(videoSender->ssrc(), discord::dave::Codec::AV1);
        break;
    case VideoCodec::Unknown:
        break;
    }
}

void VoiceClient::startVideo(const LocalVideoDescription &description)
{
    if (currentState != State::Connected || !gateway || localVideoSsrc() == 0)
        return;

    const bool starting = !localVideo;
    localVideo = description;
    if (starting) {
        if (!videoClock.isValid())
            videoClock.start();
        if (!prepareVideoSender())
            qCWarning(LogVoice) << "Video is announced but cannot be sent as" << videoCodecName(sendCodec);

        videoSendReportedUs = videoClock.nsecsElapsed() / 1000;
        videoBytesAtSendReport = videoSender ? videoSender->stats().bytesWritten : 0;
        if (!videoSendReportTimer) {
            videoSendReportTimer = new QTimer(this);
            connect(videoSendReportTimer, &QTimer::timeout, this, &VoiceClient::onVideoSendReportTick);
        }
        videoSendReportTimer->start(VIDEO_SEND_REPORT_INTERVAL_MS);
    }
    paceVideoByBitrate();
    announceLocalVideo(true);
}

void VoiceClient::describeVideo(const LocalVideoDescription &description)
{
    if (!localVideo)
        return;
    localVideo = description;
    paceVideoByBitrate();
    announceLocalVideo(true);
}

void VoiceClient::stopVideo()
{
    // Video that was never on is not said to be off either: the official client tells the
    // server nothing of its video until it has been on once.
    if (!localVideo)
        return;
    localVideo.reset();
    videoBitrate = 0;
    videoSendAwaitsKeyframe = false;

    if (videoSender)
        videoSender->clear();
    for (QTimer *timer : { videoPaceTimer, videoSendReportTimer, localKeyframeTimer }) {
        if (timer)
            timer->stop();
    }
    announceLocalVideo(false);
}

void VoiceClient::announceLocalVideo(bool on)
{
    if (!gateway)
        return;

    if (on) {
        announcedVideoStreams = localVideoStreams;
        for (int i = 0; i < announcedVideoStreams.size(); i++)
            announcedVideoStreams[i].active = i == localVideoLayer;

        StreamResolution resolution;
        const bool sourceSized = localVideo->maxWidth == 0 && localVideo->maxHeight == 0;
        resolution.type = sourceSized ? QStringLiteral("source") : QStringLiteral("fixed");
        resolution.width = localVideo->maxWidth;
        resolution.height = localVideo->maxHeight;

        StreamDescription &layer = announcedVideoStreams[localVideoLayer];
        layer.maxBitrate = localVideo->maxBitrate;
        layer.maxFramerate = localVideo->maxFramerate;
        layer.maxResolution = resolution;
    } else {
        for (StreamDescription &stream : announcedVideoStreams)
            stream.active = false;
    }

    const quint32 videoSsrc = on ? localVideoSsrc() : 0;
    LocalVideoState state;
    state.audioSsrc = localSsrc;
    state.videoSsrc = videoSsrc;
    // Always the SSRC after the video one, as the official client has it, whatever READY gave
    // the layer for its retransmissions.
    state.rtxSsrc = videoSsrc == 0 ? 0 : videoSsrc + 1;
    state.streams = announcedVideoStreams;
    gateway->sendVideo(state);
}

void VoiceClient::sendVideoFrame(const EncodedVideoFrame &frame)
{
    if (currentState != State::Connected || !encryption || !udpTransport || !localVideo || !videoSender)
        return;

    if (daveSession && !daveSession->isDaveEnabled() && !daveSession->isDowngraded()) {
        // Keys are still being agreed, and no picture goes out without them. The official
        // client fills the gap with placeholder frames; sending nothing serves as well.
        videoSendAwaitsKeyframe = true;
        return;
    }
    if (videoSendAwaitsKeyframe && !frame.keyframe)
        return;

    const qint64 now = videoClock.nsecsElapsed() / 1000;
    bool queued = false;
    if (isDaveEnabled()) {
        std::vector<uint8_t> encrypted;
        if (!daveEncryptVideo(frame.data, encrypted)) {
            videoSendAwaitsKeyframe = true;
            askForLocalKeyframe();
            return;
        }
        queued = videoSender->queueFrame(encrypted, frame.rtpTimestamp, frame.keyframe, now);
    } else {
        queued = videoSender->queueFrame(frame.data, frame.rtpTimestamp, frame.keyframe, now);
    }
    if (!queued)
        return;

    videoSendAwaitsKeyframe = false;
    startVideoPacing();
}

// The whole frame is encrypted before it is cut into packets, and comes out in another shape:
// the encryptor appends its trailer, widens the short start codes of H264 and H265, and of AV1
// drops the OBUs that are not sent and the size field of the last one.
bool VoiceClient::daveEncryptVideo(std::span<const uint8_t> frame, std::vector<uint8_t> &encrypted)
{
    auto *encryptor = daveSession->encryptor();
    // The encryptor writes its trailer without looking at how much room it was given, and the
    // size it asks for allows 64 bytes beyond the frame. A frame of many units outgrows that:
    // each NAL or OBU can add a widened start code and a pair of range numbers, nine bytes at
    // most, to a unit that may be only two bytes long.
    encrypted.resize(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size()) + frame.size() * 5);

    size_t bytesWritten = 0;
    const auto result = encryptor->Encrypt(discord::dave::MediaType::Video, videoSender->ssrc(), discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(encrypted.data(), encrypted.size()), &bytesWritten);
    if (result != discord::dave::IEncryptor::Success) {
        qCWarning(LogDave) << "DAVE video encrypt failed: result =" << static_cast<int>(result) << "frameSize =" << frame.size();
        return false;
    }
    encrypted.resize(bytesWritten);
    return true;
}

void VoiceClient::startVideoPacing()
{
    if (!videoPaceTimer) {
        videoPaceTimer = new QTimer(this);
        videoPaceTimer->setTimerType(Qt::PreciseTimer);
        connect(videoPaceTimer, &QTimer::timeout, this, &VoiceClient::onVideoPaceTick);
    }
    if (!videoPaceTimer->isActive())
        videoPaceTimer->start(VideoSendStream::PACE_INTERVAL_MS);
}

// A frame's packets written to the socket all at once overrun queues on the way and come out
// the far end as loss, so each tick lets only a share of them go.
void VoiceClient::onVideoPaceTick()
{
    if (currentState != State::Connected || !encryption || !udpTransport || !videoSender || !videoSender->hasWaiting()) {
        videoPaceTimer->stop();
        return;
    }

    videoSender->startInterval(videoClock.nsecsElapsed() / 1000);
    PacketPool::Handle packet = udpTransport->acquirePacket();
    while (videoSender->writeNext(videoClock.nsecsElapsed() / 1000, *packet)) {
        if (encryption->seal(*packet, VideoSendStream::CLEAR_SIZE))
            udpTransport->send(*packet);
    }
}

void VoiceClient::onVideoSendReportTick()
{
    if (!localVideo)
        return;
    const qint64 now = videoClock.nsecsElapsed() / 1000;

    VideoSendStats stats;
    stats.codec = videoCodecName(sendCodec);
    stats.keyframeRequests = localKeyframeRequests;
    stats.receivers = videoReceivers;
    stats.receiverEstimateBps = videoReceiverEstimateBps;

    if (videoSender) {
        if (videoSender->canReport() && currentState == State::Connected && encryption && udpTransport) {
            PacketPool::Handle packet = udpTransport->acquirePacket();
            packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
            const int size = videoSender->writeSenderReport(packet->bytes(), now, ntpTimeNow());
            if (size > 0 && packet->resize(size) && encryption->seal(*packet, RTCP_CLEAR_SIZE))
                udpTransport->send(*packet);
        }

        const VideoSendStream::Stats &sent = videoSender->stats();
        stats.packets = sent.packets;
        stats.bytes = sent.bytes;
        stats.frames = sent.frames;
        stats.keyframes = sent.keyframes;
        stats.retransmitted = sent.retransmitted;
        stats.nacksReceived = sent.nacksReceived;
        stats.lossFraction = sent.lossFraction;
        stats.roundTripMs = sent.roundTripMs;
        if (now > videoSendReportedUs)
            stats.bitrateBps = (sent.bytesWritten - videoBytesAtSendReport) * 8 * 1000000 / static_cast<quint64>(now - videoSendReportedUs);
        videoBytesAtSendReport = sent.bytesWritten;
    }
    videoSendReportedUs = now;

    emit videoSendStatsUpdated(stats);
}

void VoiceClient::receiveVideoFeedback(const ReceivedRtcp &received)
{
    const quint32 ssrc = localVideoSsrc();
    if (ssrc == 0)
        return;

    for (const ReceivedRtcp::ReceiverCount &count : received.receiverCounts) {
        if (count.ssrc == ssrc)
            noteVideoReceivers(count.receivers);
    }

    if (received.hasReceiverEstimate) {
        if (received.receiverEstimateBitsPerSecond != videoReceiverEstimateBps)
            qCDebug(LogVoice) << "Receiving side's estimate of the way from here:" << received.receiverEstimateBitsPerSecond << "bps";
        videoReceiverEstimateBps = received.receiverEstimateBitsPerSecond;
    }

    if (!localVideo || !videoSender)
        return;

    const qint64 now = videoClock.nsecsElapsed() / 1000;
    for (const ReceivedRtcp::Nack &nack : received.nacks) {
        if (nack.mediaSsrc == ssrc)
            videoSender->retransmit(nack.sequences, now);
    }
    if (videoSender->hasWaiting())
        startVideoPacing();

    for (const ReceptionReport &report : received.receptionReports) {
        if (report.ssrc == ssrc)
            videoSender->onReceptionReport(report, now);
    }

    const auto keyframeRequests = std::count(received.keyframeRequests.cbegin(), received.keyframeRequests.cend(), ssrc);
    if (keyframeRequests > 0) {
        localKeyframeRequests += static_cast<quint64>(keyframeRequests);
        askForLocalKeyframe();
    }
}

// The official client tears down and rebuilds every video receiver it has for the sender, which
// leaves each waiting for a key frame and asking for one. Starting each over does the same.
void VoiceClient::receiveVideoAfresh(quint32 ssrc)
{
    Core::Snowflake sender;
    if (const auto known = ssrcToUserIdMap.constFind(ssrc); known != ssrcToUserIdMap.constEnd())
        sender = Core::Snowflake(known.value());
    for (const auto &[videoSsrc, video] : remoteVideos) {
        if (videoSsrc == ssrc || video.rtxSsrc == ssrc)
            sender = video.userId;
    }
    qCDebug(LogVoice) << "Server asks for the video of SSRC" << ssrc << "to be received afresh";
    if (!sender.isValid())
        return;

    for (auto &[videoSsrc, video] : remoteVideos) {
        if (video.receiver && video.userId == sender)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::noteVideoReceivers(int receivers)
{
    if (receivers == videoReceivers)
        return;

    // Whoever has only now begun to receive has seen none of what was sent so far.
    const bool firstReceiver = videoReceivers <= 0 && receivers > 0;
    videoReceivers = receivers;
    emit videoReceiversChanged(receivers);
    if (firstReceiver)
        askForLocalKeyframe();
}

void VoiceClient::askForLocalKeyframe()
{
    if (!localVideo)
        return;

    const qint64 now = videoClock.elapsed();
    const qint64 wait = localKeyframeAskedMs < 0 ? 0 : localKeyframeAskedMs + LOCAL_KEYFRAME_INTERVAL_MS - now;
    if (wait > 0) {
        // Too soon after the last one: this request is passed on when the interval is over,
        // together with any others that come until then.
        if (!localKeyframeTimer) {
            localKeyframeTimer = new QTimer(this);
            localKeyframeTimer->setSingleShot(true);
            connect(localKeyframeTimer, &QTimer::timeout, this, &VoiceClient::askForLocalKeyframe);
        }
        if (!localKeyframeTimer->isActive())
            localKeyframeTimer->start(static_cast<int>(wait));
        return;
    }

    localKeyframeAskedMs = now;
    emit videoKeyframeRequested();
}

void VoiceClient::onSessionUpdated(const SessionUpdate &update)
{
    if (update.videoCodec.hasValue())
        setVideoSendCodec(update.videoCodec.get());
}

void VoiceClient::onMediaSinkWants(const MediaSinkWants &wants)
{
    const quint32 ssrc = localVideoSsrc();

    // What is said of this client's own stream counts before what is said of any, and when
    // neither says anything all of it is wanted.
    int want = 100;
    if (const auto own = wants.bySsrc.constFind(ssrc); own != wants.bySsrc.constEnd() && own.value() > 0)
        want = own.value();
    else if (wants.any && *wants.any > 0)
        want = *wants.any;

    const auto pixelCount = wants.pixelCounts.constFind(ssrc);
    emit videoWantsReceived(want, pixelCount != wants.pixelCounts.constEnd() ? pixelCount.value() : 0);
}

void VoiceClient::sendVideoWants()
{
    if (currentState != State::Connected || !gateway || videoPayloadTypes.isEmpty())
        return;
    // A stream's viewer has nothing to say until the broadcaster's video is announced, and a
    // voice connection nothing until it has been given a plan.
    if (!explicitVideoPlan && (!params.streamContext || remoteLayers.isEmpty()))
        return;

    const QByteArray serialized = plannedWants.toJson().toBytes();
    if (serialized == sentVideoWants)
        return;
    sentVideoWants = serialized;
    gateway->sendMediaSinkWants(plannedWants);
}

void VoiceClient::requestVideoKeyframeFrom(Core::Snowflake userId)
{
    for (auto &[ssrc, video] : remoteVideos) {
        if (video.receiver && video.userId == userId)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::requestVideoKeyframeFromEveryone()
{
    for (auto &[ssrc, video] : remoteVideos) {
        if (video.receiver)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::withdrawVideoCodec(VideoCodec codec)
{
    bool changed = false;
    for (Codec &offered : params.videoCodecs) {
        if (videoCodecFromName(offered.name) != codec || !offered.decode.valueOr(false))
            continue;
        offered.decode = false;
        changed = true;
    }
    if (!changed || !gateway || currentState != State::Connected)
        return;

    qCWarning(LogVoice) << "Withdrawing" << videoCodecName(codec) << "from the codecs this client decodes";
    sendOfferedCodecs();
}

void VoiceClient::startVideoTimer()
{
    if (!videoClock.isValid())
        videoClock.start();
    videoReportedMs = videoClock.elapsed();

    if (!videoTimer) {
        videoTimer = new QTimer(this);
        videoTimer->setTimerType(Qt::PreciseTimer);
        connect(videoTimer, &QTimer::timeout, this, &VoiceClient::onVideoTick);
    }
    videoTimer->start(VIDEO_TICK_MS);
}

void VoiceClient::onVideoTick()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    const qint64 now = videoClock.elapsed();
    const bool reportDue = now - videoReportedMs >= VIDEO_REPORT_INTERVAL_MS;

    struct Pending
    {
        quint32 ssrc;
        VideoReceiver::Feedback feedback;
    };
    std::vector<Pending> pending;
    std::vector<ReceptionReport> reports;
    VideoReceiveStats stats;

    for (auto &[ssrc, video] : remoteVideos) {
        if (!video.receiver)
            continue;

        VideoReceiver::Feedback feedback = video.receiver->poll(now);
        if (!feedback.nacks.empty() || feedback.keyframeRequest)
            pending.push_back({ ssrc, std::move(feedback) });

        if (!reportDue)
            continue;
        ReceptionReport report = video.receiver->receptionReport();
        if (const auto heard = senderReports.constFind(ssrc); heard != senderReports.constEnd()) {
            // The delay is in units of 1/65536 of a second.
            report.lastSenderReport = heard->compactNtp;
            report.delaySinceLastSenderReport = static_cast<quint32>((videoClock.nsecsElapsed() / 1000 - heard->arrivalUs) * 65536 / 1000000);
        }
        reports.push_back(report);
        const VideoReceiver::Stats &counters = video.receiver->stats();
        stats.codec = videoCodecName(video.receiver->codec());
        stats.packets += counters.packets;
        stats.bytes += counters.bytes;
        stats.frames += counters.frames;
        stats.keyframes += counters.keyframes;
        stats.retransmitted += counters.retransmitted;
        stats.nacked += counters.nacked;
        stats.keyframeRequests += counters.keyframeRequests;
        stats.lossFraction = std::max(stats.lossFraction, report.fractionLost / 256.0);
    }

    const bool transportDue = transportFeedback.hasPending() && now - transportReportedMs >= TRANSPORT_FEEDBACK_INTERVAL_MS;
    if (pending.empty() && reports.empty() && !transportDue) {
        if (reportDue)
            videoReportedMs = now;
        return;
    }

    // A compound packet opens with a report, so feedback sent between reports follows an empty one.
    PacketPool::Handle packet = udpTransport->acquirePacket();
    packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
    int size = writeReceiverReport(packet->bytes(), localSsrc, reports);
    if (size < 0)
        return;
    if (transportDue) {
        transportReportedMs = now;
        // One packet covers a bounded span; after a long gap it takes more than one.
        for (int i = 0; i < 4 && transportFeedback.hasPending(); i++) {
            const int written = transportFeedback.write(packet->bytes().subspan(static_cast<size_t>(size)), localSsrc, transportMediaSsrc);
            if (written <= 0)
                break;
            size += written;
        }
    }

    for (const Pending &entry : pending) {
        if (!entry.feedback.nacks.empty()) {
            const int written = writeNack(packet->bytes().subspan(static_cast<size_t>(size)), localSsrc, entry.ssrc, entry.feedback.nacks);
            if (written > 0)
                size += written;
        }
        if (entry.feedback.keyframeRequest) {
            const int written = writePli(packet->bytes().subspan(static_cast<size_t>(size)), localSsrc, entry.ssrc);
            if (written > 0)
                size += written;
        }
    }
    packet->resize(size);

    if (encryption->seal(*packet, RTCP_CLEAR_SIZE))
        udpTransport->send(*packet);

    if (reportDue) {
        videoReportedMs = now;
        stats.decryptFailures = videoDecryptFailures;
        stats.serverEstimateBps = serverEstimateBps;
        stats.serverLossFraction = serverLossFraction;
        if (!remoteVideos.empty())
            emit videoStatsUpdated(stats);
    }
}

void VoiceClient::receiveAudio(const ParsedRtp &rtp, PacketPool::Handle packet)
{
    const RtpHeader &header = rtp.header;

    // ignore our own packets
    if (header.ssrc == localSsrc)
        return;

    if (!encryption) {
        qCDebug(LogVoice) << "Received RTP but no encryption context, SSRC =" << header.ssrc;
        return;
    }

    const int packetSize = packet->size();
    if (!encryption->open(*packet, rtp.clearSize)) {
        qCDebug(LogVoice) << "Decrypt failed: SSRC =" << header.ssrc
                          << "seq =" << header.sequence
                          << "pktSize =" << packetSize
                          << "hdrSize =" << rtp.clearSize
                          << "ext =" << header.extension;
        return;
    }

    // the extension elements are encrypted with the payload and come out in front of it
    packet->dropFront(rtp.clearSize);
    if (packet->size() <= rtp.extensionSize)
        return;
    noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp.extensionSize)));
    packet->dropFront(rtp.extensionSize);

    // Stream audio arrives padded now and then; the padding is not part of the frame.
    if (header.padding) {
        const int padding = rtpPaddingSize(packet->bytes());
        if (padding < 0 || padding == packet->size())
            return;
        packet->resize(packet->size() - padding);
    }

    if (daveSession) {
        // https://daveprotocol.com/#silence-packets
        const bool isSilence = packet->size() == sizeof(Core::Audio::OPUS_SILENCE) && memcmp(packet->data(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) == 0;

        if (!isSilence) {
            if (daveSession->isDaveEnabled()) {
                if (!daveDecryptAudio(header.ssrc, packet))
                    return;
            } else if (!daveSession->isDowngraded()) {
                return;
            }
        }
    }

    emit audioReceived(header.ssrc, header.sequence, header.timestamp, QByteArray(reinterpret_cast<const char *>(packet->data()), packet->size()));
}

bool VoiceClient::daveDecryptAudio(quint32 ssrc, PacketPool::Handle &packet)
{
    uint64_t ssrcUid = ssrcToUserIdMap.value(ssrc, 0);
    auto *dec = daveSession->getOrCreateDecryptor(ssrc, ssrcUid);

    const size_t maxSize = dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Audio, packet->size());
    PacketPool::Handle plaintext = udpTransport->acquirePacket();
    if (!plaintext->resize(static_cast<int>(maxSize)))
        return false;

    size_t bytesWritten = 0;
    auto result = dec->Decrypt(discord::dave::MediaType::Audio, discord::dave::ArrayView<const uint8_t>(packet->data(), packet->size()), discord::dave::ArrayView<uint8_t>(plaintext->data(), maxSize), &bytesWritten);

    if (result != discord::dave::IDecryptor::Success) {
        static constexpr const char *kResultNames[] = {
            "Success",
            "DecryptionFailure",
            "MissingKeyRatchet",
            "InvalidNonce",
            "MissingCryptor",
        };
        int ri = static_cast<int>(result);
        const char *rn = (ri >= 0 && ri < 5) ? kResultNames[ri] : "Unknown";
        const std::span<const uint8_t> frame = packet->bytes();
        bool hasMagic = frame.size() >= 2 && frame[frame.size() - 2] == 0xFA && frame[frame.size() - 1] == 0xFA;
        qCDebug(LogDave) << "DAVE decrypt failed: SSRC =" << ssrc
                         << "result =" << rn
                         << "frameSize =" << packet->size()
                         << "hasMagicMarker =" << hasMagic;
        return false;
    }

    plaintext->resize(static_cast<int>(bytesWritten));
    packet = std::move(plaintext);
    return true;
}

bool VoiceClient::isDaveEnabled() const
{
    return daveSession && daveSession->isDaveEnabled();
}

void VoiceClient::requestVerificationCode(Core::Snowflake targetUserId,
                                          std::function<void(const QString &)> callback)
{
    if (!isDaveEnabled()) {
        callback(QString());
        return;
    }
    std::string uid = std::to_string(targetUserId);
    daveSession->getPairwiseFingerprint(uid,
                                        [cb = std::move(callback)](const std::vector<uint8_t> &fingerprint) {
                                            if (fingerprint.empty()) {
                                                cb(QString());
                                                return;
                                            }
                                            cb(formatDisplayableCode(fingerprint, 45));
                                        });
}

void VoiceClient::onSpeaking(const SpeakingData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.ssrc.get() != 0)
            ssrcToUserIdMap.insert(data.ssrc, data.userId.get());
        if (daveSession) {
            daveSession->addConnectedUser(uid);
            if (data.ssrc.get() != 0)
                daveSession->applyKeyRatchetForSsrc(data.ssrc, data.userId.get());
        }
    }
    emit speakingReceived(data);
}

void VoiceClient::onClientsConnect(const QStringList &userIds)
{
    for (const auto &id : userIds) {
        std::string uid = id.toStdString();
        connectedUserIds.insert(uid);
        if (daveSession)
            daveSession->addConnectedUser(uid);
        emit clientReachedServer(Core::Snowflake(id.toULongLong()));
    }
}

void VoiceClient::onClientConnect(const ClientConnectData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.audioSsrc.get() != 0)
            ssrcToUserIdMap.insert(data.audioSsrc, data.userId.get());
        if (daveSession)
            daveSession->addConnectedUser(uid);
    }
    applyRemoteVideo(data);
    emit clientConnected(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake uid)
{
    std::string uidStr = std::to_string(uid);
    connectedUserIds.erase(uidStr);

    if (daveSession)
        daveSession->removeConnectedUser(uidStr);

    const bool hadVideo = remoteLayers.contains(uid);
    removeRemoteVideo(uid);
    if (hadVideo) {
        emit remoteVideoLayersChanged(uid, {});
        emit remoteVideoChanged(uid, false, 0, 0, 0);
    }

    for (auto it = ssrcToUserIdMap.begin(); it != ssrcToUserIdMap.end(); ++it) {
        if (it.value() == static_cast<quint64>(uid)) {
            ssrcToUserIdMap.erase(it);
            break;
        }
    }

    emit clientDisconnected(uid);
}

void VoiceClient::onGatewayResumed()
{
    qCInfo(LogVoice) << "Voice session resumed, restoring to Connected state";

    if (localSsrc == 0 || sessionKey.isEmpty() || !encryption || !udpTransport) {
        cleanupTransport();
        if (currentState != State::Disconnected) {
            setState(State::Disconnected);
            emit disconnected();
        }
        return;
    }

    rtpEpoch = std::chrono::steady_clock::now();
    setState(State::Connected);

    sendSilence();
    if (!keepaliveTimer) {
        keepaliveTimer = new QTimer(this);
        connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
    }
    if (!keepaliveTimer->isActive())
        keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
}

void VoiceClient::onIpDiscovered(const QString &ip, int port)
{
    qCInfo(LogVoice) << "IP Discovery: external" << ip << ":" << port;

    setState(State::SelectingProtocol);

    SelectProtocolData protocol;
    protocol.address = ip;
    protocol.port = port;
    protocol.mode = selectedMode;
    protocol.codecs = offeredCodecs();
    codecsChangedSinceOffered = false;
    if (params.video)
        protocol.rtcConnectionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    gateway->sendSelectProtocol(protocol);

    setState(State::WaitingForSession);
}

void VoiceClient::onIpDiscoveryFailed(const QString &error)
{
    qCCritical(LogVoice) << "IP Discovery failed:" << error;
    stop();
}

void VoiceClient::onRelayLost(const QString &error)
{
    qCWarning(LogVoice) << "SOCKS5 relay lost, ending the connection:" << error;
    stop();
    emit disconnected();
}

void VoiceClient::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    qCInfo(LogVoice) << "Creating DAVE session, protocol version =" << protocolVersion;

    daveSession = std::make_unique<DaveSession>(params.channelId, params.userId, ssrcToUserIdMap, this);
    daveSession->setLocalSsrc(localSsrc);
    registerLocalVideoWithDave();

    for (const auto &uid : connectedUserIds)
        daveSession->addConnectedUser(uid);

    // binary
    connect(daveSession.get(), &DaveSession::sendKeyPackage,
            gateway, &VoiceGateway::sendBinaryPayload);
    connect(daveSession.get(), &DaveSession::sendCommitWelcome,
            gateway, &VoiceGateway::sendBinaryPayload);

    // json
    connect(daveSession.get(), &DaveSession::sendReadyForTransition,
            gateway, &VoiceGateway::sendDaveReadyForTransition);
    connect(daveSession.get(), &DaveSession::sendInvalidCommitWelcome,
            gateway, &VoiceGateway::sendDaveInvalidCommitWelcome);

    connect(daveSession.get(), &DaveSession::daveStateChanged,
            this, [this](bool enabled) {
                if (std::exchange(videoDroppedAwaitingDaveKeys, false))
                    requestVideoKeyframeFromEveryone();
                if (videoSendAwaitsKeyframe)
                    askForLocalKeyframe();
                if (enabled) {
                    auto auth = daveSession->lastEpochAuthenticator();
                    if (!auth.empty())
                        emit privacyCodeChanged(formatDisplayableCode(auth));
                    else
                        emit privacyCodeChanged(QString());
                } else {
                    emit privacyCodeChanged(QString());
                }
            });

    daveSession->init(protocolVersion);
}

void VoiceClient::setState(State state)
{
    if (currentState == state)
        return;

    qCDebug(LogVoice) << "VoiceClient state:" << currentState.load() << "->" << state;
    currentState = state;
    emit stateChanged(state);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
