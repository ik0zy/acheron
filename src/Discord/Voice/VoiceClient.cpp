#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"
#include "Rtcp.hpp"

#include "DaveSession.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

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

VoiceClient::VoiceClient(const ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params)
{
    qRegisterMetaType<VideoReceiveStats>();
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

    encryption.reset();

    daveSession.reset();

    delete udpTransport;
    udpTransport = nullptr;

    rtpSequence = 0;
    rtpTimestamp = 0;
    rtpEpoch = {};
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

    setState(State::Connected);
    emit connected();

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

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
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
    if (videoClock.isValid()) {
        const qint64 now = videoClock.nsecsElapsed() / 1000;
        for (const ReceivedRtcp::SenderReport &report : received.senderReports)
            senderReports.insert(report.ssrc, { report.compactNtp, now });
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
    QList<Codec> codecs = { Codec::opus() };
    codecs.append(params.videoCodecs);
    gateway->sendSessionUpdate(codecs);
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
    QList<Codec> codecs = { Codec::opus() };
    codecs.append(params.videoCodecs);
    protocol.codecs = codecs;
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
