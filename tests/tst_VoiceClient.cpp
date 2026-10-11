// A real VoiceClient, with its gateway socket, UDP transport and cipher, against a voice server
// that lives in this file: a WebSocket and a UDP socket on the loopback interface that speak
// just enough of Discord's side to bring a connection up and send it video.

#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include "Discord/Voice/VoiceClient.hpp"
#include "Discord/Voice/VoiceEncryption.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>
#include <QtEndian>

#include <memory>
#include <vector>

using namespace Acheron;
using namespace Acheron::Discord::Voice;

namespace {

constexpr int VIDEO_PAYLOAD_TYPE = 101;
constexpr int RTX_PAYLOAD_TYPE = 102;
constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;
constexpr int PACKETS_PER_FRAME = 3;

constexpr quint64 ALICE = 1001;
constexpr quint32 ALICE_SSRC = 6000;
constexpr quint32 ALICE_RTX_SSRC = 6001;
constexpr quint64 BOB = 1002;
constexpr quint32 BOB_SSRC = 7000;
constexpr quint32 BOB_RTX_SSRC = 7001;

struct NackSeen
{
    quint32 mediaSsrc = 0;
    quint16 sequence = 0;

    bool operator==(const NackSeen &) const = default;
};

class FakeVoiceServer : public QObject
{
public:
    FakeVoiceServer()
    {
        gateway.listen(QHostAddress::LocalHost);
        media.bind(QHostAddress::LocalHost);
        connect(&gateway, &QTcpServer::newConnection, this, &FakeVoiceServer::onGatewayConnection);
        connect(&media, &QUdpSocket::readyRead, this, &FakeVoiceServer::onDatagrams);
    }

    [[nodiscard]] QString endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(gateway.serverPort()); }

    [[nodiscard]] QList<QJsonObject> received(int opcode) const { return receivedByOpcode.value(opcode); }

    void send(int opcode, const QJsonObject &data)
    {
        const QByteArray text = QJsonDocument(QJsonObject{ { "op", opcode }, { "d", data } }).toJson(QJsonDocument::Compact);
        QByteArray frame;
        frame.append(char(0x81));
        if (text.size() < 126) {
            frame.append(char(text.size()));
        } else {
            frame.append(char(126));
            frame.append(char(text.size() >> 8));
            frame.append(char(text.size() & 0xFF));
        }
        frame.append(text);
        socket->write(frame);
    }

    void announceVideo(quint64 userId, quint32 ssrc, quint32 rtxSsrc)
    {
        const QJsonObject stream{ { "type", "video" }, { "rid", "100" }, { "ssrc", qint64(ssrc) }, { "rtx_ssrc", qint64(rtxSsrc) }, { "active", true }, { "quality", 100 } };
        send(12, QJsonObject{ { "user_id", QString::number(userId) }, { "audio_ssrc", qint64(ssrc - 1000) }, { "video_ssrc", qint64(ssrc) }, { "streams", QJsonArray{ stream } } });
    }

    // A VP8 payload the depacketizer takes: the descriptor says whether a picture starts here,
    // and the frame header's first bit whether it is a key frame.
    static std::vector<uint8_t> payload(bool first, bool keyframe, uint8_t tag)
    {
        return { static_cast<uint8_t>(first ? 0x10 : 0x00), static_cast<uint8_t>(keyframe ? 0x00 : 0x01), tag, tag };
    }

    void sendPacket(quint32 ssrc, int payloadType, quint16 sequence, quint32 timestamp, bool marker, std::span<const uint8_t> body)
    {
        RtpHeader header;
        header.extension = true;
        header.marker = marker;
        header.payloadType = static_cast<uint8_t>(payloadType);
        header.sequence = sequence;
        header.timestamp = timestamp;
        header.ssrc = ssrc;

        uint8_t numbered[2];
        qToBigEndian<quint16>(nextTransportSequence++, numbered);
        const RtpExtension elements[] = { { TRANSPORT_SEQUENCE_EXTENSION, numbered } };
        uint8_t extensions[8];
        const int extensionSize = writeRtpExtensions(elements, extensions);

        PacketBuffer packet;
        const int clearSize = writeRtpHeader(header, { packet.data(), static_cast<size_t>(packet.tailroom()) }, extensionSize / 4);
        packet.resize(clearSize);
        packet.append({ extensions, static_cast<size_t>(extensionSize) });
        packet.append(body);
        cipher->seal(packet, clearSize);
        media.writeDatagram(reinterpret_cast<const char *>(packet.data()), packet.size(), clientAddress, clientPort);
    }

    // Frame number `index` of a sender whose numbering starts at `base`, leaving out packet `skip`.
    void sendFrame(quint32 ssrc, int index, quint16 base, bool keyframe, int skip = -1)
    {
        for (int i = 0; i < PACKETS_PER_FRAME; i++) {
            if (i == skip)
                continue;
            const quint16 sequence = static_cast<quint16>(base + index * PACKETS_PER_FRAME + i);
            const std::vector<uint8_t> body = payload(i == 0, keyframe, static_cast<uint8_t>(sequence));
            sendPacket(ssrc, VIDEO_PAYLOAD_TYPE, sequence, 3000u * static_cast<quint32>(index), i == PACKETS_PER_FRAME - 1, body);
        }
    }

    void retransmit(quint32 rtxSsrc, quint16 rtxSequence, int index, quint16 base, bool keyframe, int packetOfFrame)
    {
        const quint16 original = static_cast<quint16>(base + index * PACKETS_PER_FRAME + packetOfFrame);
        std::vector<uint8_t> body(2);
        qToBigEndian<quint16>(original, body.data());
        const std::vector<uint8_t> repaired = payload(packetOfFrame == 0, keyframe, static_cast<uint8_t>(original));
        body.insert(body.end(), repaired.begin(), repaired.end());
        sendPacket(rtxSsrc, RTX_PAYLOAD_TYPE, rtxSequence, 3000u * static_cast<quint32>(index), packetOfFrame == PACKETS_PER_FRAME - 1, body);
    }

    [[nodiscard]] quint16 transportSequencesSent() const { return nextTransportSequence; }

    QList<NackSeen> nacks;
    QList<quint32> keyframeRequests;
    QList<quint32> receptionReports;
    // One past the highest of the server's own packet numbers the client has reported on.
    int acknowledgedUpTo = 0;
    int undecryptable = 0;

private:
    void onGatewayConnection()
    {
        socket = gateway.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &FakeVoiceServer::onGatewayBytes);
    }

    void onGatewayBytes()
    {
        incoming.append(socket->readAll());

        if (!upgraded) {
            const int headersEnd = incoming.indexOf("\r\n\r\n");
            if (headersEnd < 0)
                return;
            QByteArray key;
            for (const QByteArray &line : incoming.left(headersEnd).split('\n')) {
                if (line.toLower().startsWith("sec-websocket-key:"))
                    key = line.mid(line.indexOf(':') + 1).trimmed();
            }
            incoming.remove(0, headersEnd + 4);
            const QByteArray accept = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
            socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
            upgraded = true;
            send(8, QJsonObject{ { "v", 9 }, { "heartbeat_interval", 5000 } });
        }

        for (;;) {
            if (incoming.size() < 2)
                return;
            const auto *bytes = reinterpret_cast<const uint8_t *>(incoming.constData());
            const int opcode = bytes[0] & 0x0F;
            const bool masked = bytes[1] & 0x80;
            qint64 length = bytes[1] & 0x7F;
            int offset = 2;
            if (length == 126) {
                if (incoming.size() < 4)
                    return;
                length = qFromBigEndian<quint16>(bytes + 2);
                offset = 4;
            }
            const int maskSize = masked ? 4 : 0;
            if (incoming.size() < offset + maskSize + length)
                return;

            QByteArray body = incoming.mid(offset + maskSize, static_cast<int>(length));
            if (masked) {
                for (int i = 0; i < body.size(); i++)
                    body[i] = static_cast<char>(body[i] ^ incoming[offset + (i % 4)]);
            }
            incoming.remove(0, offset + maskSize + static_cast<int>(length));

            constexpr int TEXT = 1;
            constexpr int CLOSE = 8;
            if (opcode == CLOSE) {
                socket->write(QByteArray::fromHex("8800"));
                socket->disconnectFromHost();
                return;
            }
            if (opcode == TEXT)
                onGatewayMessage(QJsonDocument::fromJson(body).object());
        }
    }

    void onGatewayMessage(const QJsonObject &message)
    {
        constexpr int IDENTIFY = 0;
        constexpr int SELECT_PROTOCOL = 1;
        constexpr int HEARTBEAT = 3;

        const int opcode = message["op"].toInt();
        const QJsonObject data = message["d"].toObject();
        receivedByOpcode[opcode].append(data);

        const QString mode = encryptionModeToString(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE);
        if (opcode == IDENTIFY) {
            send(2, QJsonObject{ { "ssrc", 777 }, { "ip", "127.0.0.1" }, { "port", media.localPort() }, { "modes", QJsonArray{ mode } }, { "streams", QJsonArray() } });
        } else if (opcode == SELECT_PROTOCOL) {
            const QByteArray key(32, char(0x42));
            QJsonArray keyNumbers;
            for (char byte : key)
                keyNumbers.append(static_cast<uint8_t>(byte));
            cipher = std::make_unique<VoiceEncryption>(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE, key);
            send(4, QJsonObject{ { "mode", mode }, { "secret_key", keyNumbers }, { "audio_codec", "opus" }, { "video_codec", "VP8" }, { "dave_protocol_version", 0 } });
        } else if (opcode == HEARTBEAT) {
            send(6, QJsonObject{ { "t", data["t"] } });
        }
    }

    void onDatagrams()
    {
        while (media.hasPendingDatagrams()) {
            QByteArray datagram(static_cast<int>(media.pendingDatagramSize()), Qt::Uninitialized);
            QHostAddress from;
            quint16 fromPort = 0;
            media.readDatagram(datagram.data(), datagram.size(), &from, &fromPort);
            const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(datagram.constData()), static_cast<size_t>(datagram.size()));

            constexpr int IP_DISCOVERY_SIZE = 74;
            if (!cipher && datagram.size() == IP_DISCOVERY_SIZE) {
                clientAddress = from;
                clientPort = fromPort;
                QByteArray reply(IP_DISCOVERY_SIZE, '\0');
                qToBigEndian<quint16>(2, reply.data());
                qToBigEndian<quint16>(70, reply.data() + 2);
                memcpy(reply.data() + 4, datagram.constData() + 4, 4);
                memcpy(reply.data() + 8, "127.0.0.1", 9);
                qToBigEndian<quint16>(fromPort, reply.data() + 72);
                media.writeDatagram(reply, from, fromPort);
                continue;
            }
            if (cipher && classifyPacket(bytes) == PacketKind::Rtcp)
                onFeedback(bytes);
        }
    }

    void onFeedback(std::span<const uint8_t> sealed)
    {
        PacketBuffer packet;
        packet.append(sealed);
        if (!cipher->open(packet, RTCP_CLEAR_SIZE)) {
            undecryptable++;
            return;
        }

        constexpr int RECEIVER_REPORT = 201;
        constexpr int TRANSPORT_FEEDBACK = 205;
        constexpr int PAYLOAD_FEEDBACK = 206;
        constexpr int GENERIC_NACK = 1;
        constexpr int TRANSPORT_WIDE = 15;
        constexpr int PICTURE_LOSS = 1;

        const std::span<const uint8_t> compound = packet.bytes();
        size_t offset = 0;
        while (offset + 4 <= compound.size()) {
            const uint8_t *part = compound.data() + offset;
            const int format = part[0] & 0x1F;
            const int type = part[1];
            const size_t size = (static_cast<size_t>(qFromBigEndian<quint16>(part + 2)) + 1) * 4;
            if (offset + size > compound.size())
                return;

            if (type == RECEIVER_REPORT) {
                for (int block = 0; block < format; block++)
                    receptionReports.append(qFromBigEndian<quint32>(part + 8 + block * 24));
            } else if (type == TRANSPORT_FEEDBACK && format == GENERIC_NACK) {
                const quint32 mediaSsrc = qFromBigEndian<quint32>(part + 8);
                for (size_t item = 12; item + 4 <= size; item += 4) {
                    const quint16 first = qFromBigEndian<quint16>(part + item);
                    const quint16 following = qFromBigEndian<quint16>(part + item + 2);
                    nacks.append({ mediaSsrc, first });
                    for (int bit = 0; bit < 16; bit++) {
                        if (following & (1 << bit))
                            nacks.append({ mediaSsrc, static_cast<quint16>(first + bit + 1) });
                    }
                }
            } else if (type == TRANSPORT_FEEDBACK && format == TRANSPORT_WIDE) {
                const int base = qFromBigEndian<quint16>(part + 12);
                const int count = qFromBigEndian<quint16>(part + 14);
                acknowledgedUpTo = qMax(acknowledgedUpTo, base + count);
            } else if (type == PAYLOAD_FEEDBACK && format == PICTURE_LOSS) {
                keyframeRequests.append(qFromBigEndian<quint32>(part + 8));
            }
            offset += size;
        }
    }

    QTcpServer gateway;
    QTcpSocket *socket = nullptr;
    QByteArray incoming;
    bool upgraded = false;
    QHash<int, QList<QJsonObject>> receivedByOpcode;

    QUdpSocket media;
    QHostAddress clientAddress;
    quint16 clientPort = 0;
    std::unique_ptr<VoiceEncryption> cipher;
    quint16 nextTransportSequence = 0;
};

struct FrameSeen
{
    quint64 userId = 0;
    quint32 timestamp = 0;
    bool keyframe = false;
};

// A voice connection that offers VP8 and has reached the connected state.
struct Call
{
    Call()
    {
        ConnectionParams params;
        params.endpoint = server.endpoint();
        params.gatewayScheme = QStringLiteral("ws");
        params.token = QStringLiteral("token");
        params.serverId = Core::Snowflake(1);
        params.channelId = Core::Snowflake(2);
        params.userId = Core::Snowflake(3);
        params.sessionId = QStringLiteral("session");
        params.video = true;
        params.sendsAudio = false;

        Codec vp8;
        vp8.name = QStringLiteral("VP8");
        vp8.type = QStringLiteral("video");
        vp8.priority = 1000;
        vp8.payloadType = VIDEO_PAYLOAD_TYPE;
        vp8.rtxPayloadType = RTX_PAYLOAD_TYPE;
        vp8.encode = false;
        vp8.decode = true;
        params.videoCodecs = { vp8 };

        client = std::make_unique<VoiceClient>(params);
        client->setVideoFrameHandler([this](Core::Snowflake userId, VideoCodec, EncodedVideoFrame frame) {
            frames.append({ static_cast<quint64>(userId), frame.rtpTimestamp, frame.keyframe });
        });
        QObject::connect(client.get(), &VoiceClient::remoteVideoChanged, client.get(), [this](Core::Snowflake userId, bool active) {
            if (active)
                announced.append(static_cast<quint64>(userId));
        });
        client->start();
    }

    ~Call() { client->stop(); }

    [[nodiscard]] bool isUp() const { return client->state() == VoiceClient::State::Connected; }

    // Alice and Bob both have a camera on, and both are received.
    void receiveAliceAndBob()
    {
        server.announceVideo(ALICE, ALICE_SSRC, ALICE_RTX_SSRC);
        server.announceVideo(BOB, BOB_SSRC, BOB_RTX_SSRC);
        QTRY_COMPARE(announced.size(), 2);

        MediaSinkWants wants;
        wants.any = 100;
        client->setVideoPlan(wants, { ALICE_SSRC, BOB_SSRC });
    }

    [[nodiscard]] int framesFrom(quint64 userId) const
    {
        return static_cast<int>(std::count_if(frames.cbegin(), frames.cend(), [userId](const FrameSeen &frame) { return frame.userId == userId; }));
    }

    FakeVoiceServer server;
    std::unique_ptr<VoiceClient> client;
    QList<FrameSeen> frames;
    QList<quint64> announced;
};

} // namespace

class TestVoiceClient : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(VoiceEncryption::initialize()); }

    void connectsAndSaysWhatItCanReceive();
    void framesReachTheHandlerUnderTheirSender();
    void aLostPacketIsAskedForAndTakenFromTheRetransmission();
    void aKeyframeRequestGoesToThatSenderAlone();
    void aStrayPacketLeavesTheStreamRunning();
    void everyPacketTheServerNumberedIsReportedBack();
};

void TestVoiceClient::connectsAndSaysWhatItCanReceive()
{
    Call call;
    QTRY_VERIFY(call.isUp());

    const QList<QJsonObject> identify = call.server.received(0);
    QCOMPARE(identify.size(), 1);
    QCOMPARE(identify[0]["server_id"].toString(), QStringLiteral("1"));
    QCOMPARE(identify[0]["video"].toBool(), true);

    const QList<QJsonObject> selectProtocol = call.server.received(1);
    QCOMPARE(selectProtocol.size(), 1);
    const QJsonArray codecs = selectProtocol[0]["codecs"].toArray();
    QCOMPARE(codecs.size(), 2);
    QCOMPARE(codecs[0].toObject()["name"].toString(), QStringLiteral("opus"));
    QCOMPARE(codecs[1].toObject()["name"].toString(), QStringLiteral("VP8"));
    QCOMPARE(codecs[1].toObject()["payload_type"].toInt(), VIDEO_PAYLOAD_TYPE);
    QCOMPARE(codecs[1].toObject()["rtx_payload_type"].toInt(), RTX_PAYLOAD_TYPE);
    QCOMPARE(selectProtocol[0]["data"].toObject()["mode"].toString(), encryptionModeToString(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE));
}

void TestVoiceClient::framesReachTheHandlerUnderTheirSender()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    call.server.sendFrame(ALICE_SSRC, 1, 100, false);
    QTRY_COMPARE(call.frames.size(), 3);

    QCOMPARE(call.framesFrom(ALICE), 2);
    QCOMPARE(call.framesFrom(BOB), 1);
    QVERIFY(call.frames[0].keyframe);
    QCOMPARE(call.server.undecryptable, 0);

    // What the server is told to send follows the plan.
    const QList<QJsonObject> wants = call.server.received(15);
    QVERIFY(!wants.isEmpty());
    QCOMPARE(wants.last()["any"].toInt(), 100);
}

void TestVoiceClient::aLostPacketIsAskedForAndTakenFromTheRetransmission()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    constexpr quint16 BASE = 65530;
    constexpr int LOST_PACKET = 1;
    call.server.sendFrame(ALICE_SSRC, 0, BASE, true);
    call.server.sendFrame(ALICE_SSRC, 1, BASE, false, LOST_PACKET);
    call.server.sendFrame(ALICE_SSRC, 2, BASE, false);
    QTRY_COMPARE(call.frames.size(), 1);

    const NackSeen expected{ ALICE_SSRC, static_cast<quint16>(BASE + PACKETS_PER_FRAME + LOST_PACKET) };
    QTRY_VERIFY(call.server.nacks.contains(expected));
    QVERIFY(std::all_of(call.server.nacks.cbegin(), call.server.nacks.cend(), [&expected](const NackSeen &nack) { return nack == expected; }));

    call.server.retransmit(ALICE_RTX_SSRC, 0, 1, BASE, false, LOST_PACKET);
    QTRY_COMPARE(call.frames.size(), 3);
    QVERIFY(!call.server.keyframeRequests.contains(ALICE_SSRC));
}

void TestVoiceClient::aKeyframeRequestGoesToThatSenderAlone()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    QTRY_COMPARE(call.frames.size(), 2);
    QTest::qWait(100);
    call.server.keyframeRequests.clear();

    // Alice's decoder could not use what it was given.
    call.client->requestVideoKeyframeFrom(Core::Snowflake(ALICE));
    QTRY_VERIFY(call.server.keyframeRequests.contains(ALICE_SSRC));

    // Bob's video carries on from where it was; Alice's waits for her key frame.
    call.server.sendFrame(ALICE_SSRC, 1, 100, false);
    call.server.sendFrame(BOB_SSRC, 1, 40000, false);
    QTRY_COMPARE(call.framesFrom(BOB), 2);
    QTest::qWait(VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS + 200);
    QCOMPARE(call.framesFrom(ALICE), 1);
    QVERIFY(!call.server.keyframeRequests.contains(BOB_SSRC));

    call.server.sendFrame(ALICE_SSRC, 2, 100, true);
    QTRY_COMPARE(call.framesFrom(ALICE), 2);
}

void TestVoiceClient::aStrayPacketLeavesTheStreamRunning()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 20000, true);
    QTRY_COMPARE(call.frames.size(), 1);
    QTest::qWait(100);
    call.server.keyframeRequests.clear();

    // Something from long ago turns up.
    const std::vector<uint8_t> old = FakeVoiceServer::payload(true, false, 0);
    call.server.sendPacket(ALICE_SSRC, VIDEO_PAYLOAD_TYPE, 100, 0, true, old);
    call.server.sendFrame(ALICE_SSRC, 1, 20000, false);
    QTRY_COMPARE(call.frames.size(), 2);
    QTest::qWait(200);
    QVERIFY(call.server.keyframeRequests.isEmpty());
}

void TestVoiceClient::everyPacketTheServerNumberedIsReportedBack()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    // Padding under a payload type nobody plays is numbered like everything else.
    constexpr int PROBE_PAYLOAD_TYPE = 96;
    const std::vector<uint8_t> padding(40, 0);
    call.server.sendPacket(ALICE_RTX_SSRC, PROBE_PAYLOAD_TYPE, 9, 0, false, padding);

    QTRY_COMPARE(call.server.acknowledgedUpTo, static_cast<int>(call.server.transportSequencesSent()));
    QTRY_VERIFY(call.server.receptionReports.contains(ALICE_SSRC) && call.server.receptionReports.contains(BOB_SSRC));
    QCOMPARE(call.server.undecryptable, 0);
}

QTEST_GUILESS_MAIN(TestVoiceClient)
#include "tst_VoiceClient.moc"
