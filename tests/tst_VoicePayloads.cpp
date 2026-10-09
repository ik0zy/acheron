// Tests for the voice gateway payloads that carry video. Expected bytes are what the official
// desktop client's JSON.stringify produces for the same values (key order included), taken from
// the canary client's RTC control socket; see docs/video-re/re-voice-gateway-video.md.

#include "Discord/Voice/VoiceEntities.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Acheron;
using namespace Acheron::Discord::Voice;

namespace {

QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

StreamDescription offeredStream(const QString &type, const QString &rid, int quality)
{
    StreamDescription stream;
    stream.type = type;
    stream.rid = rid;
    stream.quality = quality;
    return stream;
}

VoiceIdentifyData identifyFor(quint64 serverId, quint64 channelId)
{
    VoiceIdentifyData identify;
    identify.serverId = Core::Snowflake(serverId);
    identify.channelId = Core::Snowflake(channelId);
    identify.userId = Core::Snowflake(300);
    identify.sessionId = QStringLiteral("session");
    identify.token = QStringLiteral("token");
    return identify;
}

Codec videoCodec(const QString &name, int index, bool encode, bool decode)
{
    Codec codec;
    codec.name = name;
    codec.type = QStringLiteral("video");
    codec.priority = (index + 1) * 1000;
    codec.payloadType = 101 + 2 * index;
    codec.rtxPayloadType = 102 + 2 * index;
    codec.encode = encode;
    codec.decode = decode;
    return codec;
}

} // namespace

class TestVoicePayloads : public QObject
{
    Q_OBJECT
private slots:
    void identifyWithoutVideoOmitsVideoKeys();
    void identifyVoiceConnectionOffersTwoCameraLayers();
    void identifyStreamConnectionOffersOneScreenLayer();

    void selectProtocolAudioOnly();
    void selectProtocolWithVideoCodecs();

    void localVideoStateKeyOrder();
    void localVideoStateCameraOff();

    void sinkWantsDefault();
    void sinkWantsPutSsrcsBeforeAny();
    void sinkWantsForStreamViewer();
    void sinkWantsParse();

    void readyParsesAssignedStreams();
    void readyWithoutStreams();
    void videoFromServerParsesUserSsrcs();
    void sessionDescriptionParsesVideoFields();
    void sessionUpdateParsesOnlyWhatIsPresent();
};

void TestVoicePayloads::identifyWithoutVideoOmitsVideoKeys()
{
    QCOMPARE(identifyFor(100, 200).toJson().toBytes(),
             QByteArray(R"({"server_id":"100","channel_id":"200","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1})"));
}

void TestVoicePayloads::identifyVoiceConnectionOffersTwoCameraLayers()
{
    VoiceIdentifyData identify = identifyFor(100, 200);
    identify.video = true;
    identify.streams = QList<StreamDescription>{ offeredStream("video", "100", 100), offeredStream("video", "50", 50) };

    QCOMPARE(identify.toJson().toBytes(),
             QByteArray(R"({"server_id":"100","channel_id":"200","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1,"video":true,"streams":[{"type":"video","rid":"100","quality":100},{"type":"video","rid":"50","quality":50}]})"));
}

void TestVoicePayloads::identifyStreamConnectionOffersOneScreenLayer()
{
    VoiceIdentifyData identify = identifyFor(900, 899);
    identify.video = true;
    identify.streams = QList<StreamDescription>{ offeredStream("screen", "100", 100) };

    QCOMPARE(identify.toJson().toBytes(),
             QByteArray(R"({"server_id":"900","channel_id":"899","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1,"video":true,"streams":[{"type":"screen","rid":"100","quality":100}]})"));
}

void TestVoicePayloads::selectProtocolAudioOnly()
{
    SelectProtocolData protocol;
    protocol.address = QStringLiteral("203.0.113.7");
    protocol.port = 50123;
    protocol.mode = QStringLiteral("aead_aes256_gcm_rtpsize");
    protocol.codecs = QList<Codec>{ Codec::opus() };

    QCOMPARE(protocol.toJson().toBytes(),
             QByteArray(R"({"protocol":"udp","data":{"address":"203.0.113.7","port":50123,"mode":"aead_aes256_gcm_rtpsize"},"address":"203.0.113.7","port":50123,"mode":"aead_aes256_gcm_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120}]})"));
}

void TestVoicePayloads::selectProtocolWithVideoCodecs()
{
    SelectProtocolData protocol;
    protocol.address = QStringLiteral("203.0.113.7");
    protocol.port = 50123;
    protocol.mode = QStringLiteral("aead_xchacha20_poly1305_rtpsize");
    protocol.codecs = QList<Codec>{ Codec::opus(), videoCodec("H264", 0, false, true), videoCodec("VP8", 1, false, true) };
    protocol.rtcConnectionId = QStringLiteral("0b0f3c2e-8f5c-4d5e-9a57-3c1f6a2d9e10");
    protocol.experiments = QStringList{ QStringLiteral("fixed_keyframe_interval") };

    QCOMPARE(protocol.toJson().toBytes(),
             QByteArray(R"({"protocol":"udp","data":{"address":"203.0.113.7","port":50123,"mode":"aead_xchacha20_poly1305_rtpsize"},"address":"203.0.113.7","port":50123,"mode":"aead_xchacha20_poly1305_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"H264","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":false,"decode":true},{"name":"VP8","type":"video","priority":2000,"payload_type":103,"rtx_payload_type":104,"encode":false,"decode":true}],"rtc_connection_id":"0b0f3c2e-8f5c-4d5e-9a57-3c1f6a2d9e10","experiments":["fixed_keyframe_interval"]})"));
}

void TestVoicePayloads::localVideoStateKeyOrder()
{
    StreamResolution resolution;
    resolution.type = QStringLiteral("fixed");
    resolution.width = 1280;
    resolution.height = 720;

    StreamDescription stream;
    stream.type = QStringLiteral("video");
    stream.rid = QStringLiteral("100");
    stream.ssrc = 4001u;
    stream.active = true;
    stream.quality = 100;
    stream.rtxSsrc = 4002u;
    stream.maxBitrate = 2500000;
    stream.maxFramerate = 30;
    stream.maxResolution = resolution;

    LocalVideoState state;
    state.audioSsrc = 4000u;
    state.videoSsrc = 4001u;
    state.rtxSsrc = 4002u;
    state.streams = QList<StreamDescription>{ stream };

    QCOMPARE(state.toJson().toBytes(),
             QByteArray(R"({"audio_ssrc":4000,"video_ssrc":4001,"rtx_ssrc":4002,"streams":[{"type":"video","rid":"100","ssrc":4001,"active":true,"quality":100,"rtx_ssrc":4002,"max_bitrate":2500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]})"));
}

void TestVoicePayloads::localVideoStateCameraOff()
{
    LocalVideoState state;
    state.audioSsrc = 4000u;
    state.videoSsrc = 0u;
    state.rtxSsrc = 0u;

    QCOMPARE(state.toJson().toBytes(), QByteArray(R"({"audio_ssrc":4000,"video_ssrc":0,"rtx_ssrc":0,"streams":[]})"));
}

void TestVoicePayloads::sinkWantsDefault()
{
    MediaSinkWants wants;
    wants.any = 100;
    QCOMPARE(wants.toJson().toBytes(), QByteArray(R"({"any":100})"));
}

void TestVoicePayloads::sinkWantsPutSsrcsBeforeAny()
{
    MediaSinkWants wants;
    wants.any = 50;
    wants.bySsrc.insert(9000, 100);
    wants.bySsrc.insert(2000, 0);

    QCOMPARE(wants.toJson().toBytes(), QByteArray(R"({"2000":0,"9000":100,"any":50})"));
}

void TestVoicePayloads::sinkWantsForStreamViewer()
{
    MediaSinkWants wants;
    wants.bySsrc.insert(7001, 100);
    wants.pixelCounts.insert(7001, 1920 * 1080);
    QCOMPARE(wants.toJson().toBytes(), QByteArray(R"({"7001":100,"pixelCounts":{"7001":2073600}})"));

    MediaSinkWants hidden;
    hidden.bySsrc.insert(7001, 0);
    QCOMPARE(hidden.toJson().toBytes(), QByteArray(R"({"7001":0})"));
}

void TestVoicePayloads::sinkWantsParse()
{
    const MediaSinkWants wants = MediaSinkWants::fromJson(parse(R"({"any":70,"4001":100,"4003":0,"pixelCounts":{"4001":921600}})"));

    QCOMPARE(wants.any.value_or(-1), 70);
    QCOMPARE(wants.bySsrc.size(), 2);
    QCOMPARE(wants.bySsrc.value(4001), 100);
    QCOMPARE(wants.bySsrc.value(4003), 0);
    QCOMPARE(wants.pixelCounts.value(4001), 921600);

    const MediaSinkWants bare = MediaSinkWants::fromJson(parse(R"({"4001":30})"));
    QVERIFY(!bare.any.has_value());
    QCOMPARE(bare.bySsrc.value(4001), 30);
    QVERIFY(bare.pixelCounts.isEmpty());
}

void TestVoicePayloads::readyParsesAssignedStreams()
{
    const VoiceReady ready = VoiceReady::fromJson(parse(R"({"ssrc":4000,"ip":"203.0.113.7","port":50004,"modes":["aead_aes256_gcm_rtpsize","aead_xchacha20_poly1305_rtpsize"],"streams":[{"type":"video","rid":"100","ssrc":4001,"rtx_ssrc":4002,"active":false,"quality":100},{"type":"video","rid":"50","ssrc":4003,"rtx_ssrc":4004,"active":false,"quality":50}]})"));

    QCOMPARE(ready.ssrc.get(), quint32(4000));
    QVERIFY(ready.streams.hasValue());
    QCOMPARE(ready.streams->size(), 2);

    const StreamDescription &high = ready.streams->at(0);
    QCOMPARE(high.type.get(), QStringLiteral("video"));
    QCOMPARE(high.rid.get(), QStringLiteral("100"));
    QCOMPARE(high.ssrc.get(), quint32(4001));
    QCOMPARE(high.rtxSsrc.get(), quint32(4002));
    QCOMPARE(high.active.get(), false);
    QCOMPARE(high.quality.get(), 100);
    QVERIFY(!high.maxBitrate.hasValue());
    QVERIFY(!high.maxResolution.hasValue());

    const StreamDescription &low = ready.streams->at(1);
    QCOMPARE(low.rid.get(), QStringLiteral("50"));
    QCOMPARE(low.ssrc.get(), quint32(4003));
    QCOMPARE(low.rtxSsrc.get(), quint32(4004));
}

void TestVoicePayloads::readyWithoutStreams()
{
    const VoiceReady ready = VoiceReady::fromJson(parse(R"({"ssrc":4000,"ip":"203.0.113.7","port":50004,"modes":["aead_aes256_gcm_rtpsize"]})"));

    QCOMPARE(ready.ssrc.get(), quint32(4000));
    QVERIFY(!ready.streams.hasValue());
}

void TestVoicePayloads::videoFromServerParsesUserSsrcs()
{
    const ClientConnectData video = ClientConnectData::fromJson(parse(R"({"user_id":"123456789012345678","audio_ssrc":5000,"video_ssrc":5001,"streams":[{"type":"video","rid":"100","ssrc":5001,"rtx_ssrc":5002,"active":true,"quality":100,"max_bitrate":2500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]})"));

    QCOMPARE(quint64(video.userId.get()), quint64(123456789012345678));
    QCOMPARE(video.audioSsrc.get(), quint32(5000));
    QCOMPARE(video.videoSsrc.get(), quint32(5001));
    QVERIFY(video.streams.hasValue());
    QCOMPARE(video.streams->size(), 1);

    const StreamDescription &stream = video.streams->at(0);
    QCOMPARE(stream.active.get(), true);
    QCOMPARE(stream.maxBitrate.get(), 2500000);
    QCOMPARE(stream.maxFramerate.get(), 30);
    QVERIFY(stream.maxResolution.hasValue());
    QCOMPARE(stream.maxResolution->type.get(), QStringLiteral("fixed"));
    QCOMPARE(stream.maxResolution->width.get(), 1280);
    QCOMPARE(stream.maxResolution->height.get(), 720);

    // Camera off: the same message with a zero video SSRC and no streams.
    const ClientConnectData off = ClientConnectData::fromJson(parse(R"({"user_id":"123456789012345678","audio_ssrc":5000,"video_ssrc":0})"));
    QCOMPARE(off.videoSsrc.get(), quint32(0));
    QVERIFY(!off.streams.hasValue());
}

void TestVoicePayloads::sessionDescriptionParsesVideoFields()
{
    const SessionDescription description = SessionDescription::fromJson(parse(R"({"audio_codec":"opus","video_codec":"H264","media_session_id":"abc123","mode":"aead_aes256_gcm_rtpsize","secret_key":[1,2,3],"keyframe_interval":0,"dave_protocol_version":1})"));

    QCOMPARE(description.mode.get(), QStringLiteral("aead_aes256_gcm_rtpsize"));
    QCOMPARE(description.secretKey.get(), QByteArray("\x01\x02\x03", 3));
    QCOMPARE(description.audioCodec.get(), QStringLiteral("opus"));
    QCOMPARE(description.videoCodec.get(), QStringLiteral("H264"));
    QCOMPARE(description.mediaSessionId.get(), QStringLiteral("abc123"));
    QCOMPARE(description.keyframeInterval.get(), 0);
    QCOMPARE(description.daveProtocolVersion.get(), 1);
}

void TestVoicePayloads::sessionUpdateParsesOnlyWhatIsPresent()
{
    const SessionUpdate codecChange = SessionUpdate::fromJson(parse(R"({"video_codec":"VP8"})"));
    QCOMPARE(codecChange.videoCodec.get(), QStringLiteral("VP8"));
    QVERIFY(!codecChange.audioCodec.hasValue());
    QVERIFY(!codecChange.mediaSessionId.hasValue());
    QVERIFY(!codecChange.keyframeInterval.hasValue());

    const SessionUpdate interval = SessionUpdate::fromJson(parse(R"({"keyframe_interval":5000})"));
    QCOMPARE(interval.keyframeInterval.get(), 5000);
    QVERIFY(!interval.videoCodec.hasValue());
}

QTEST_GUILESS_MAIN(TestVoicePayloads)
#include "tst_VoicePayloads.moc"
