#include "Core/Video/CameraSender.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Logging.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Discord/Client.hpp"

#include <optional>
#include <utility>

namespace Acheron {
namespace Core {

using Discord::Voice::LocalVideoDescription;
using Discord::Voice::VideoCodec;
using Discord::Voice::VideoSendStats;
using Discord::Voice::VoiceClient;

CameraSender::CameraSender(Discord::Client *client, Audio::VoiceManager *voice, StreamBroadcaster *screenShare, Permission allowedHere, QObject *parent)
    : QObject(parent), client(client), voice(voice), screenShare(screenShare), allowedHere(std::move(allowedHere))
{
    clock.start();

    planTimer.setSingleShot(true);
    connect(&planTimer, &QTimer::timeout, this, &CameraSender::applyPlan);

    codecTimeout.setSingleShot(true);
    codecTimeout.setInterval(CODEC_TIMEOUT_MS);
    connect(&codecTimeout, &QTimer::timeout, this, &CameraSender::onCodecTimedOut);

    connect(voice, &Audio::VoiceManager::rtcClientCreated, this, &CameraSender::onRtcClientCreated);
    connect(voice, &Audio::VoiceManager::rtcClientClosing, this, &CameraSender::onRtcClientClosing);
    connect(voice, &Audio::VoiceManager::voiceDisconnected, this, &CameraSender::onVoiceDisconnected);
    connect(voice, &Audio::VoiceManager::channelVoiceMemberChanged, this, &CameraSender::onVoiceMembershipChanged);
    connect(voice, &Audio::VoiceManager::participantVoiceStateChanged, this, &CameraSender::onOwnVoiceStateChanged);
    connect(voice, &Audio::VoiceManager::speakingChanged, this, &CameraSender::onSpeakingChanged);
    connect(screenShare, &StreamBroadcaster::stateChanged, this, &CameraSender::onScreenShareChanged);

    if (voice->rtcClient())
        onRtcClientCreated(voice->rtcClient());
}

CameraSender::~CameraSender()
{
    QObject::disconnect(framesToRtc);
    stopPipeline();
}

QList<VideoCodec> CameraSender::codecsToOffer(const QList<VideoCodec> &encodable)
{
    return Media::cameraCodecs(encodable, Media::videoEncoderFor(VideoCodec::AV1));
}

bool CameraSender::canTurnOn() const
{
    return Video::cameraCaptureAvailable() && voice->isConnected() && allowedHere();
}

void CameraSender::reset()
{
    if (isOn())
        fail(tr("Your camera was turned off because the connection to Discord was restarted."), false);
}

void CameraSender::setState(State state)
{
    if (currentState == state)
        return;
    currentState = state;
    emit stateChanged();
}

void CameraSender::clearFailure()
{
    if (currentState != State::Failed)
        return;
    failureText.clear();
    setState(State::Off);
}

CameraSender::Stats CameraSender::stats() const
{
    Stats now;
    now.encoder = encoderStats;
    now.network = networkStats;
    now.targetBitrate = asked.bitrate;
    now.receivers = receivers;
    now.servedWant = plan.servedWant();
    now.mutedFramerate = pipeline && plan.countsAsMuted(clock.elapsed());
    now.encodingSuspended = encodingSuspended;
    return now;
}

void CameraSender::turnOn(const QString &wantedDeviceId)
{
    if (isOn()) {
        if (wantedDeviceId == deviceId)
            return;
        deviceId = wantedDeviceId;
        if (pipeline)
            startPipeline();
        return;
    }

    failureText.clear();
    if (!canTurnOn()) {
        fail(tr("You cannot turn on your camera here."));
        return;
    }

    deviceId = wantedDeviceId;
    cameraChannelId = voice->currentChannelId();
    plan.start(clock.elapsed());
    plan.setSelfMuted(voice->isSelfMuted());
    plan.setSpeaking(speaking, clock.elapsed());
    plan.setSharingScreen(screenShare->isSharing());
    setState(State::Starting);
    sendVoiceState();

    StreamBroadcaster::whenEncodersKnown(this, [this, thisAttempt = ++attempt](const QList<VideoCodec> &codecs) {
        if (thisAttempt == attempt)
            onEncodersKnown(codecs);
    });
}

void CameraSender::onEncodersKnown(const QList<VideoCodec> &codecs)
{
    if (codecs.isEmpty()) {
        fail(tr("This machine has no video encoder that Discord accepts, so it cannot send a camera."));
        return;
    }

    encodersKnown = true;
    voice->setEncodableVideoCodecs(codecsToOffer(codecs));
    sendOnceReady();
}

void CameraSender::turnOff()
{
    end(true, State::Off);
}

void CameraSender::end(bool tellGateway, State endState)
{
    if (!isOn())
        return;

    attempt++;
    encodersKnown = false;
    codecTimeout.stop();
    planTimer.stop();

    if (rtc)
        QMetaObject::invokeMethod(rtc, [connection = rtc] { connection->stopVideo(); });
    videoAnnounced = false;
    stopPipeline();
    latestPreview = QImage();
    networkStats = {};

    const Snowflake left = std::exchange(cameraChannelId, Snowflake::Invalid);
    currentState = endState;

    // Moved to another channel, the account's voice state there still shows the camera when
    // the server did the moving.
    const Snowflake here = voice->currentChannelId();
    if (tellGateway && here.isValid() && (here == left || voice->isSelfVideo()))
        sendVoiceState();

    emit stateChanged();
}

void CameraSender::fail(const QString &reason, bool tellGateway)
{
    qCWarning(LogVoice) << "Camera turned off:" << reason;
    failureText = reason;
    if (isOn())
        end(tellGateway, State::Failed);
    else
        setState(State::Failed);
    emit failed(reason);
}

void CameraSender::sendVoiceState()
{
    const Snowflake channelId = voice->currentChannelId();
    client->sendVoiceStateUpdate(voice->currentGuildId(), channelId, voice->isSelfMuted(), voice->isSelfDeafened(), isOnIn(channelId));
}

void CameraSender::onCodecTimedOut()
{
    fail(tr("Your camera could not be turned on: Discord named no video codec this machine can encode."));
}

// The call is over, and with it whatever was said of the camera in it. Nothing is sent to the
// gateway: a voice state update that names the channel would join it again.
void CameraSender::onVoiceDisconnected()
{
    speaking = false;
    end(false, State::Off);
    clearFailure();
}

void CameraSender::onVoiceMembershipChanged()
{
    if (isOn() && voice->currentChannelId() != cameraChannelId)
        end(true, State::Off);
}

void CameraSender::onOwnVoiceStateChanged()
{
    plan.setSelfMuted(voice->isSelfMuted());
    applyPlan();
}

void CameraSender::onSpeakingChanged(bool nowSpeaking)
{
    speaking = nowSpeaking;
    plan.setSpeaking(nowSpeaking, clock.elapsed());
    applyPlan();
}

void CameraSender::onScreenShareChanged()
{
    plan.setSharingScreen(screenShare->isSharing());
    applyPlan();
}

void CameraSender::onRtcClientCreated(VoiceClient *created)
{
    onRtcClientClosing();

    rtc = created;
    const quint64 serial = ++connectionSerial;

    // The client's signals come from its own thread, so each arrives here later, and may
    // arrive after its connection has been replaced: the number tells those apart.
    connect(rtc, &VoiceClient::stateChanged, this, [this, serial](VoiceClient::State state) {
        if (serial == connectionSerial)
            onRtcStateChanged(state);
    });
    connect(rtc, &VoiceClient::videoSendCodecChanged, this, [this, serial](VideoCodec codec) {
        if (serial == connectionSerial)
            onSendCodecChanged(codec);
    });
    connect(rtc, &VoiceClient::videoSendStatsUpdated, this, [this, serial](const VideoSendStats &sent) {
        if (serial == connectionSerial)
            onSendStats(sent);
    });
    connect(rtc, &VoiceClient::videoKeyframeRequested, this, [this, serial] {
        if (serial == connectionSerial)
            onKeyframeRequested();
    });
    connect(rtc, &VoiceClient::videoReceiversChanged, this, [this, serial](int count) {
        if (serial == connectionSerial)
            onReceiversChanged(count);
    });
    connect(rtc, &VoiceClient::videoWantsReceived, this, [this, serial](int want, int) {
        if (serial == connectionSerial)
            onWantsReceived(want);
    });
    sendFramesToRtc();
}

void CameraSender::onRtcClientClosing()
{
    if (!rtc)
        return;

    QObject::disconnect(framesToRtc);
    QObject::disconnect(rtc, nullptr, this, nullptr);
    connectionSerial++;
    rtc = nullptr;
    rtcConnected = false;
    videoAnnounced = false;
    sendCodec = VideoCodec::Unknown;
    receivers = -1;
    codecTimeout.stop();

    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onRtcStateChanged(VoiceClient::State state)
{
    rtcConnected = state == VoiceClient::State::Connected;
    // A connection that had to be made again has forgotten what it was told of the camera.
    videoAnnounced = false;
    if (rtcConnected) {
        sendOnceReady();
        return;
    }

    codecTimeout.stop();
    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onSendCodecChanged(VideoCodec codec)
{
    sendCodec = codec;
    sendOnceReady();
}

void CameraSender::onSendStats(const VideoSendStats &sent)
{
    networkStats = sent;
    if (!pipelineStarted)
        return;
    plan.setReceiverLimit(static_cast<qint64>(sent.receiverEstimateBps));
    plan.reportLoss(sent.lossFraction, clock.elapsed());
    applyPlan();
}

void CameraSender::onKeyframeRequested()
{
    if (pipeline)
        pipeline->requestKeyframe();
}

void CameraSender::onReceiversChanged(int count)
{
    receivers = count;
    applySuspension();
}

void CameraSender::onWantsReceived(int want)
{
    plan.setWant(want);
    applyPlan();
}

void CameraSender::sendOnceReady()
{
    if (!isOn() || !encodersKnown || !rtcConnected)
        return;

    if (sendCodec == VideoCodec::Unknown) {
        // A connection made before the encoders were known hears of them late, and the server
        // then names the codec to use. One it never names cannot be waited for forever.
        if (!codecTimeout.isActive())
            codecTimeout.start();
        return;
    }
    codecTimeout.stop();

    if (!pipeline || pipelineCodec != sendCodec)
        startPipeline();
    else if (pipelineStarted && !videoAnnounced)
        announceVideo();
}

void CameraSender::startPipeline()
{
    stopPipeline();

    pipeline = std::make_unique<Video::CameraPipeline>();
    pipelineCodec = sendCodec;
    const quint64 serial = ++pipelineSerial;
    connect(pipeline.get(), &Video::CameraPipeline::started, this, [this, serial](const QSize &size, int) {
        if (serial == pipelineSerial)
            onPipelineStarted(size);
    });
    connect(pipeline.get(), &Video::CameraPipeline::previewReady, this, [this, serial](const QImage &preview) {
        if (serial == pipelineSerial)
            onPreviewReady(preview);
    });
    connect(pipeline.get(), &Video::CameraPipeline::failed, this, [this, serial](const QString &reason) {
        if (serial == pipelineSerial)
            onPipelineFailed(reason);
    });
    connect(pipeline.get(), &Video::CameraPipeline::statsUpdated, this, [this, serial](const Video::BroadcastStats &encoder) {
        if (serial == pipelineSerial)
            onPipelineStats(encoder);
    });
    sendFramesToRtc();

    asked = plan.advance(clock.elapsed());
    Video::CameraSettings settings;
    settings.deviceId = deviceId;
    settings.codec = sendCodec;
    settings.encodeSize = asked.size;
    settings.framerate = asked.framerate;
    settings.bitrate = asked.bitrate;
    // The most the camera is ever sent at, whichever row of the ladder it starts on.
    settings.maxBitrate = Video::CAMERA_MAX_BITRATE;
    pipeline->start(settings);
    applyPlan();
}

void CameraSender::stopPipeline()
{
    if (!pipeline)
        return;

    pipelineSerial++;
    planTimer.stop();
    QObject::disconnect(framesToRtc);
    pipeline->stop();
    pipeline.reset();

    pipelineStarted = false;
    encodingSuspended = false;
    encoderStats = {};
}

void CameraSender::sendFramesToRtc()
{
    QObject::disconnect(framesToRtc);
    if (!pipeline || !rtc)
        return;

    framesToRtc = connect(pipeline.get(), &Video::CameraPipeline::frameEncoded, rtc, [connection = rtc](const Video::EncodedFramePointer &frame) {
        connection->sendVideoFrame(*frame);
    });
}

void CameraSender::onPipelineStarted(const QSize &encodedSize)
{
    qCInfo(LogVoice) << "Camera is encoding at" << encodedSize;
    pipelineStarted = true;
    sendOnceReady();
    applySuspension();
}

void CameraSender::onPreviewReady(const QImage &preview)
{
    const bool first = latestPreview.isNull();
    latestPreview = preview;
    emit previewChanged(preview);
    if (first)
        emit stateChanged();
}

void CameraSender::onPipelineFailed(const QString &reason)
{
    fail(tr("Your camera was turned off: %1").arg(reason));
}

void CameraSender::onPipelineStats(const Video::BroadcastStats &encoder)
{
    encoderStats = encoder;
}

void CameraSender::announceVideo()
{
    LocalVideoDescription description;
    description.maxWidth = Video::CAMERA_CAPTURE_SIZE.width();
    description.maxHeight = Video::CAMERA_CAPTURE_SIZE.height();
    description.maxFramerate = Video::CAMERA_FRAMERATE;
    description.maxBitrate = Video::CAMERA_MAX_BITRATE;

    videoAnnounced = true;
    QMetaObject::invokeMethod(rtc, [connection = rtc, description, bitrate = asked.bitrate] {
        connection->startVideo(description);
        connection->setVideoBitrate(bitrate);
    });
    applySuspension();
    // What was encoded before the client took the announcement was dropped by it, and that
    // included the frame every viewer needs first.
    pipeline->requestKeyframe();
    setState(State::On);
}

// The size, frame rate and bitrate change what is encoded and nothing else: the official
// client announces none of them.
void CameraSender::applyPlan()
{
    planTimer.stop();
    if (!pipeline)
        return;

    const qint64 now = clock.elapsed();
    const Video::CameraEncoding wanted = plan.advance(now);
    // The bitrate first, so that an encoder reopened for a new size opens at the rate that
    // goes with it.
    if (wanted.bitrate != asked.bitrate) {
        pipeline->setBitrate(wanted.bitrate);
        if (rtc)
            QMetaObject::invokeMethod(rtc, [connection = rtc, bitrate = wanted.bitrate] { connection->setVideoBitrate(bitrate); });
    }
    if (wanted.size != asked.size || wanted.framerate != asked.framerate)
        pipeline->setEncoding(wanted.size, wanted.framerate);
    asked = wanted;

    if (const std::optional<qint64> wait = plan.nextChangeIn(now))
        planTimer.start(static_cast<int>(*wait));
}

// Nothing is encoded while nobody is there to receive it, as in the official client; the
// camera stays open and its preview live. Only once the pipeline has started, though: it is
// not promised to report a start while it encodes nothing.
void CameraSender::applySuspension()
{
    const bool suspend = pipelineStarted && (!videoAnnounced || receivers == 0);
    if (suspend == encodingSuspended)
        return;
    encodingSuspended = suspend;
    pipeline->setEncodingSuspended(suspend);
}

} // namespace Core
} // namespace Acheron
