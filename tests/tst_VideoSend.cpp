// Tests for sending video over RTP: the packetizers, the sender that numbers, paces and repeats
// their packets, and the RTCP that goes with it. What the packetizers write is read back by the
// depacketizers the receive path uses, and the end-to-end encryption cases put libdave's
// encryptor in front of them and its decryptor behind.

#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoPacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "Discord/Voice/VideoSender.hpp"

#include <bytes/bytes.h>
#include <dave/dave_interfaces.h>

#include <QTest>
#include <QtEndian>

#include <optional>
#include <random>

using namespace Acheron::Discord::Voice;

namespace {

using Bytes = std::vector<uint8_t>;
using Payloads = std::vector<Bytes>;

constexpr uint32_t SSRC = 0x1234;
constexpr uint32_t RTX_SSRC = 0x1299;
constexpr uint8_t PAYLOAD_TYPE = 101;
constexpr uint8_t RTX_PAYLOAD_TYPE = 102;

// What is left for a payload once the header, the extensions and the room for a retransmission
// are taken out of the 1200 bytes a packet may have.
constexpr int PACKET_OVERHEAD = 12 + 4 + 12;
constexpr int MAX_PAYLOAD = 1200 - PACKET_OVERHEAD - 2;
constexpr int VP8_DESCRIPTOR = 4;
constexpr int VP8_ROOM = MAX_PAYLOAD - VP8_DESCRIPTOR;
constexpr int FU_HEADER = 2;

constexpr int NAL_TYPE_MASK = 0x1F;
constexpr int NAL_SLICE = 0x41;
constexpr int NAL_IDR = 0x65;
constexpr int NAL_SPS = 0x67;
constexpr int NAL_PPS = 0x68;
constexpr int STAP_A = 24;
constexpr int FU_A = 28;

constexpr qint64 MS = 1000;
constexpr qint64 SECOND = 1000 * MS;

// Bytes that can never form a start code, so a NAL made of them needs no escaping.
Bytes filler(size_t size, uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> byte(1, 255);
    Bytes bytes(size);
    for (uint8_t &value : bytes)
        value = static_cast<uint8_t>(byte(generator));
    return bytes;
}

Bytes vp8Frame(bool keyframe, size_t size, uint32_t seed)
{
    Bytes frame = filler(size, seed);
    frame[0] = static_cast<uint8_t>(keyframe ? (frame[0] & ~1) : (frame[0] | 1));
    return frame;
}

// One NAL of `size` bytes in all, behind a start code of `startCodeSize` bytes.
void appendNal(Bytes &frame, uint8_t header, size_t size, uint32_t seed, int startCodeSize = 4)
{
    frame.insert(frame.end(), static_cast<size_t>(startCodeSize - 1), 0);
    frame.push_back(1);
    frame.push_back(header);
    Bytes body = filler(size - 1, seed);
    // A slice that begins its picture, which is what marks the start of a key frame.
    if (!body.empty())
        body[0] |= 0x80;
    frame.insert(frame.end(), body.begin(), body.end());
}

Bytes h264Keyframe(size_t pictureSize, int startCodeSize = 4)
{
    Bytes frame;
    appendNal(frame, NAL_SPS, 21, 1, startCodeSize);
    appendNal(frame, NAL_PPS, 7, 2, startCodeSize);
    appendNal(frame, NAL_IDR, pictureSize, 3, startCodeSize);
    return frame;
}

bool reassemble(VideoCodec codec, const Payloads &payloads, Bytes &frame)
{
    std::vector<std::span<const uint8_t>> spans;
    for (const Bytes &payload : payloads)
        spans.emplace_back(payload);
    return assembleFrame(codec, spans, frame);
}

bool noneLargerThan(const Payloads &payloads, size_t limit)
{
    return std::all_of(payloads.begin(), payloads.end(), [limit](const Bytes &payload) { return !payload.empty() && payload.size() <= limit; });
}

class FixedKeyRatchet : public discord::dave::IKeyRatchet
{
public:
    discord::dave::EncryptionKey GetKey(discord::dave::KeyGeneration generation) noexcept override
    {
        std::vector<uint8_t> key(16, 0x5A);
        key[0] = static_cast<uint8_t>(generation);
        return discord::dave::EncryptionKey(std::move(key));
    }
    void DeleteKey(discord::dave::KeyGeneration) noexcept override {}
};

// Encrypted the way VoiceClient does it: the output sized by the encryptor's own bound.
Bytes encryptFrame(discord::dave::Codec codec, const Bytes &frame)
{
    auto encryptor = discord::dave::CreateEncryptor();
    encryptor->SetKeyRatchet(std::make_unique<FixedKeyRatchet>());
    encryptor->AssignSsrcToCodec(SSRC, codec);

    Bytes encrypted(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size()));
    size_t written = 0;
    const auto result = encryptor->Encrypt(discord::dave::MediaType::Video, SSRC, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(encrypted.data(), encrypted.size()), &written);
    if (result != discord::dave::IEncryptor::Success)
        return {};
    encrypted.resize(written);
    return encrypted;
}

bool decryptFrame(const Bytes &encrypted, Bytes &frame)
{
    auto decryptor = discord::dave::CreateDecryptor();
    decryptor->TransitionToKeyRatchet(std::make_unique<FixedKeyRatchet>());

    frame.resize(decryptor->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, encrypted.size()));
    size_t written = 0;
    const auto result = decryptor->Decrypt(discord::dave::MediaType::Video, discord::dave::ArrayView<const uint8_t>(encrypted.data(), encrypted.size()), discord::dave::ArrayView<uint8_t>(frame.data(), frame.size()), &written);
    frame.resize(written);
    return result == discord::dave::IDecryptor::Success;
}

// One packet as the sender wrote it, taken apart the way a receiver would.
struct Written
{
    RtpHeader header;
    int clearSize = 0;
    int size = 0;
    Bytes extensions;
    Bytes payload;

    [[nodiscard]] Bytes extension(uint8_t id) const
    {
        const std::span<const uint8_t> value = findRtpExtension(extensions, id);
        return Bytes(value.begin(), value.end());
    }

    [[nodiscard]] uint16_t transportSequence() const
    {
        const Bytes value = extension(VideoSendStream::TRANSPORT_SEQUENCE_EXTENSION);
        return value.size() == 2 ? qFromBigEndian<uint16_t>(value.data()) : 0;
    }

    [[nodiscard]] uint32_t sendTime() const
    {
        const Bytes value = extension(VideoSendStream::ABS_SEND_TIME_EXTENSION);
        return value.size() == 3 ? (uint32_t(value[0]) << 16) | (uint32_t(value[1]) << 8) | value[2] : 0xFFFFFFFF;
    }
};

std::optional<Written> takeApart(const PacketBuffer &packet)
{
    const std::optional<ParsedRtp> rtp = parseRtp(packet.bytes());
    if (!rtp || packet.size() < rtp->clearSize + rtp->extensionSize)
        return std::nullopt;

    Written written;
    written.header = rtp->header;
    written.clearSize = rtp->clearSize;
    written.size = packet.size();
    const std::span<const uint8_t> body = packet.bytes().subspan(static_cast<size_t>(rtp->clearSize));
    written.extensions.assign(body.begin(), body.begin() + rtp->extensionSize);
    written.payload.assign(body.begin() + rtp->extensionSize, body.end());
    return written;
}

// One pacing interval's worth of packets.
std::vector<Written> interval(VideoSendStream &sender, qint64 nowUs)
{
    std::vector<Written> out;
    PacketBuffer packet;
    sender.startInterval();
    while (sender.writeNext(nowUs, packet)) {
        if (const std::optional<Written> written = takeApart(packet))
            out.push_back(*written);
    }
    return out;
}

// Everything that is waiting, over as many intervals as it takes.
std::vector<Written> drain(VideoSendStream &sender, qint64 nowUs)
{
    std::vector<Written> out;
    while (sender.hasWaiting()) {
        const std::vector<Written> some = interval(sender, nowUs);
        if (some.empty())
            break;
        out.insert(out.end(), some.begin(), some.end());
    }
    return out;
}

Payloads payloadsOf(const std::vector<Written> &packets)
{
    Payloads payloads;
    for (const Written &packet : packets)
        payloads.push_back(packet.payload);
    return payloads;
}

constexpr VideoSendStream::Numbering FIRST{ 65533, 500, 65534, 0x7FFF };

VideoSendStream vp8Sender()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(VideoCodec::VP8, PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    return sender;
}

void append32(Bytes &out, uint32_t value)
{
    uint8_t bytes[4];
    qToBigEndian(value, bytes);
    out.insert(out.end(), bytes, bytes + 4);
}

void append16(Bytes &out, uint16_t value)
{
    uint8_t bytes[2];
    qToBigEndian(value, bytes);
    out.insert(out.end(), bytes, bytes + 2);
}

// An RTCP packet from its parts; the length field is worked out from the body.
Bytes rtcpPacket(uint8_t countOrFormat, uint8_t type, const Bytes &body)
{
    Bytes packet = { static_cast<uint8_t>(0x80 | countOrFormat), type };
    append16(packet, static_cast<uint16_t>(body.size() / 4));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

} // namespace

class TestVideoSend : public QObject
{
    Q_OBJECT
private slots:
    void vp8FramesComeBackWhole_data();
    void vp8FramesComeBackWhole();
    void vp8PacketsOfAFrameShareItsPictureId();
    void h264UnitsComeBackWhole_data();
    void h264UnitsComeBackWhole();
    void h264KeyframeAggregatesItsParameterSets();
    void h264KeyframeThatFitsIsOneAggregate();
    void h264ShortStartCodesComeBackLong();
    void whatCannotBePacketizedGivesNothing();
    void encryptedFramesSurvivePacketizing_data();
    void encryptedFramesSurvivePacketizing();

    void packetsAreNumberedMarkedAndExtended();
    void theSendTimeIsSecondsInFixedPoint();
    void noPacketIsLargerThanTheLimit_data();
    void noPacketIsLargerThanTheLimit();
    void aFrameLeavesInSharesAndInOrder();
    void aBacklogLeavesFaster();
    void nothingLeavesOutsideAnInterval();
    void aLostPacketIsSentAgainOnTheRetransmissionStream();
    void retransmissionsGoAheadOfNewPackets();
    void whatWasNeverSentIsNotSentAgain();
    void theHistoryEndsAfterASecond();
    void withoutARetransmissionPayloadTypeNothingIsSentAgain();
    void aReceiverRepairsALossFromTheHistory();
    void changingTheCodecKeepsTheNumbering();
    void clearingForgetsWhatWaited();
    void framesAreRefusedWithoutACodec();

    void theSenderReportStatesWhatWasSent();
    void aReceptionReportGivesLossAndRoundTrip();
    void feedbackForTheSenderIsRead();
    void theReceiverCountIsRead();
};

void TestVideoSend::vp8FramesComeBackWhole_data()
{
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("packets");

    QTest::newRow("one byte") << 1 << 1;
    QTest::newRow("exactly one packet") << VP8_ROOM << 1;
    QTest::newRow("one byte over") << VP8_ROOM + 1 << 2;
    QTest::newRow("several packets") << 5 * VP8_ROOM + 300 << 6;
    QTest::newRow("a large key frame") << 250000 << (250000 + VP8_ROOM - 1) / VP8_ROOM;
}

void TestVideoSend::vp8FramesComeBackWhole()
{
    QFETCH(int, size);
    QFETCH(int, packets);

    const Bytes frame = vp8Frame(true, static_cast<size_t>(size), 20);
    VideoPacketizer packetizer(VideoCodec::VP8);
    const Payloads payloads = packetizer.packetize(frame, MAX_PAYLOAD);

    QCOMPARE(static_cast<int>(payloads.size()), packets);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));
    // The pieces are of about one size, so a frame one byte over does not end in a one-byte packet.
    const auto [smallest, largest] = std::minmax_element(payloads.begin(), payloads.end(), [](const Bytes &a, const Bytes &b) { return a.size() < b.size(); });
    QVERIFY(largest->size() - smallest->size() <= 1);

    // Only the first packet says that a picture starts in it.
    QVERIFY(startsKeyframe(VideoCodec::VP8, payloads.front()));
    for (size_t i = 0; i < payloads.size(); i++)
        QCOMPARE((payloads[i][0] & 0x10) != 0, i == 0);

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::VP8, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::vp8PacketsOfAFrameShareItsPictureId()
{
    const auto pictureId = [](const Bytes &payload) { return ((payload[2] & 0x7F) << 8) | payload[3]; };

    VideoPacketizer packetizer(VideoCodec::VP8, 0x7FFE);
    for (int expected : { 0x7FFE, 0x7FFF, 0, 1 }) {
        const Payloads payloads = packetizer.packetize(vp8Frame(false, 3000, 21), MAX_PAYLOAD);
        QCOMPARE(payloads.size(), size_t(3));
        for (const Bytes &payload : payloads) {
            // X, then I with the M bit: a picture id of fifteen bits and nothing else.
            QCOMPARE(payload[0] & 0xEF, 0x80);
            QCOMPARE(payload[1], uint8_t(0x80));
            QVERIFY(payload[2] & 0x80);
            QCOMPARE(pictureId(payload), expected);
        }
    }
}

void TestVideoSend::h264UnitsComeBackWhole_data()
{
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("packets");

    QTest::newRow("one byte") << 1 << 1;
    QTest::newRow("exactly one packet") << MAX_PAYLOAD << 1;
    QTest::newRow("one byte over") << MAX_PAYLOAD + 1 << 2;
    QTest::newRow("several packets") << 6000 << 6;
    QTest::newRow("a large picture") << 250000 << (250000 - 1 + MAX_PAYLOAD - FU_HEADER - 1) / (MAX_PAYLOAD - FU_HEADER);
}

void TestVideoSend::h264UnitsComeBackWhole()
{
    QFETCH(int, size);
    QFETCH(int, packets);

    Bytes frame;
    appendNal(frame, NAL_SLICE, static_cast<size_t>(size), 30);

    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads payloads = packetizer.packetize(frame, MAX_PAYLOAD);

    QCOMPARE(static_cast<int>(payloads.size()), packets);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    if (packets == 1) {
        // The unit travels as it is, without its start code.
        QCOMPARE(payloads[0], Bytes(frame.begin() + 4, frame.end()));
    } else {
        for (size_t i = 0; i < payloads.size(); i++) {
            // The unit's importance stays in the indicator and its type moves to the second byte.
            QCOMPARE(int(payloads[i][0]), (NAL_SLICE & ~NAL_TYPE_MASK) | FU_A);
            QCOMPARE(payloads[i][1] & NAL_TYPE_MASK, NAL_SLICE & NAL_TYPE_MASK);
            QCOMPARE((payloads[i][1] & 0x80) != 0, i == 0);
            QCOMPARE((payloads[i][1] & 0x40) != 0, i + 1 == payloads.size());
        }
    }

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::h264KeyframeAggregatesItsParameterSets()
{
    const Bytes frame = h264Keyframe(5000);
    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads payloads = packetizer.packetize(frame, MAX_PAYLOAD);

    QVERIFY(payloads.size() > 2);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    // SPS and PPS share the first packet, each behind its length, and nothing else is in it.
    const Bytes &aggregate = payloads[0];
    QCOMPARE(aggregate[0] & NAL_TYPE_MASK, STAP_A);
    QCOMPARE(aggregate[0] & 0x60, NAL_SPS & 0x60);
    QCOMPARE(aggregate.size(), size_t(1 + 2 + 21 + 2 + 7));
    QCOMPARE((aggregate[1] << 8) | aggregate[2], 21);
    QCOMPARE(int(aggregate[3]), NAL_SPS);
    QCOMPARE((aggregate[24] << 8) | aggregate[25], 7);
    QCOMPARE(int(aggregate[26]), NAL_PPS);
    QVERIFY(startsWithParameterSets(VideoCodec::H264, aggregate));

    // The picture does not fit beside them and is cut into fragments.
    for (size_t i = 1; i < payloads.size(); i++) {
        QCOMPARE(payloads[i][0] & NAL_TYPE_MASK, FU_A);
        QCOMPARE(payloads[i][1] & NAL_TYPE_MASK, NAL_IDR & NAL_TYPE_MASK);
    }
    QVERIFY(payloads[1][1] & 0x80);
    QVERIFY(payloads.back()[1] & 0x40);

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::h264KeyframeThatFitsIsOneAggregate()
{
    const Bytes frame = h264Keyframe(200);
    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads payloads = packetizer.packetize(frame, MAX_PAYLOAD);

    QCOMPARE(payloads.size(), size_t(1));
    QCOMPARE(payloads[0][0] & NAL_TYPE_MASK, STAP_A);
    QCOMPARE(payloads[0].size(), size_t(1 + 2 + 21 + 2 + 7 + 2 + 200));
    QVERIFY(startsKeyframe(VideoCodec::H264, payloads[0]));

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloads, reassembled));
    QCOMPARE(reassembled, frame);

    // One unit more than fits: the aggregate closes and the last unit goes by itself.
    Bytes larger = frame;
    appendNal(larger, NAL_SLICE, MAX_PAYLOAD - 100, 31);
    const Payloads two = packetizer.packetize(larger, MAX_PAYLOAD);
    QCOMPARE(two.size(), size_t(2));
    QCOMPARE(two[0], payloads[0]);
    QCOMPARE(int(two[1][0]), NAL_SLICE);
    QVERIFY(reassemble(VideoCodec::H264, two, reassembled));
    QCOMPARE(reassembled, larger);
}

void TestVideoSend::h264ShortStartCodesComeBackLong()
{
    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads fromShort = packetizer.packetize(h264Keyframe(5000, 3), MAX_PAYLOAD);
    const Payloads fromLong = packetizer.packetize(h264Keyframe(5000, 4), MAX_PAYLOAD);
    QVERIFY(!fromShort.empty());
    QCOMPARE(fromShort, fromLong);

    // Of the zero bytes in front of a start code one is the start code's; more are the unit's own.
    const Bytes frame = { 0, 0, 1, NAL_SLICE, 0x80, 0x55, 0, 0, 0, 0, 1, NAL_SLICE, 0x80, 0x66 };
    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, packetizer.packetize(frame, MAX_PAYLOAD), reassembled));
    QCOMPARE(reassembled, (Bytes{ 0, 0, 0, 1, NAL_SLICE, 0x80, 0x55, 0, 0, 0, 0, 1, NAL_SLICE, 0x80, 0x66 }));
}

void TestVideoSend::whatCannotBePacketizedGivesNothing()
{
    QVERIFY(VideoPacketizer::supports(VideoCodec::H264));
    QVERIFY(VideoPacketizer::supports(VideoCodec::VP8));
    QVERIFY(!VideoPacketizer::supports(VideoCodec::H265));
    QVERIFY(!VideoPacketizer::supports(VideoCodec::AV1));
    QVERIFY(!VideoPacketizer::supports(VideoCodec::Unknown));

    const Bytes frame = h264Keyframe(5000);
    for (VideoCodec codec : { VideoCodec::H265, VideoCodec::AV1, VideoCodec::Unknown })
        QVERIFY(VideoPacketizer(codec).packetize(frame, MAX_PAYLOAD).empty());

    QVERIFY(VideoPacketizer(VideoCodec::VP8).packetize({}, MAX_PAYLOAD).empty());
    QVERIFY(VideoPacketizer(VideoCodec::H264).packetize({}, MAX_PAYLOAD).empty());
    // Not Annex B: there is no start code to find a unit behind.
    QVERIFY(VideoPacketizer(VideoCodec::H264).packetize(filler(500, 40), MAX_PAYLOAD).empty());
    QVERIFY(VideoPacketizer(VideoCodec::H264).packetize(frame, 3).empty());
}

void TestVideoSend::encryptedFramesSurvivePacketizing_data()
{
    QTest::addColumn<int>("codec");
    QTest::addColumn<Bytes>("frame");
    QTest::addColumn<Bytes>("decrypted");

    QTest::newRow("H264 key frame") << int(VideoCodec::H264) << h264Keyframe(5000) << h264Keyframe(5000);
    // The encryptor widens the start codes, so what a receiver decrypts has long ones.
    QTest::newRow("H264 key frame, short start codes") << int(VideoCodec::H264) << h264Keyframe(5000, 3) << h264Keyframe(5000, 4);
    QTest::newRow("VP8 key frame") << int(VideoCodec::VP8) << vp8Frame(true, 5000, 50) << vp8Frame(true, 5000, 50);
    QTest::newRow("VP8 delta frame") << int(VideoCodec::VP8) << vp8Frame(false, 900, 51) << vp8Frame(false, 900, 51);
}

void TestVideoSend::encryptedFramesSurvivePacketizing()
{
    QFETCH(int, codec);
    QFETCH(Bytes, frame);
    QFETCH(Bytes, decrypted);
    const VideoCodec videoCodec = static_cast<VideoCodec>(codec);

    const Bytes encrypted = encryptFrame(videoCodec == VideoCodec::H264 ? discord::dave::Codec::H264 : discord::dave::Codec::VP8, frame);
    QVERIFY(encrypted.size() > frame.size());

    VideoPacketizer packetizer(videoCodec);
    const Payloads payloads = packetizer.packetize(encrypted, MAX_PAYLOAD);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    // The decryptor has to be handed exactly what the encryptor produced.
    Bytes reassembled;
    QVERIFY(reassemble(videoCodec, payloads, reassembled));
    QCOMPARE(reassembled, encrypted);

    Bytes plain;
    QVERIFY(decryptFrame(reassembled, plain));
    QCOMPARE(plain, decrypted);
}

void TestVideoSend::packetsAreNumberedMarkedAndExtended()
{
    VideoSendStream sender = vp8Sender();
    const Bytes first = vp8Frame(true, 3000, 60);
    const Bytes second = vp8Frame(false, 2000, 61);
    QVERIFY(sender.queueFrame(first, 90000, true, 0));
    QVERIFY(sender.queueFrame(second, 93000, false, 0));

    const std::vector<Written> packets = drain(sender, 0);
    QCOMPARE(packets.size(), size_t(5));

    for (size_t i = 0; i < packets.size(); i++) {
        const Written &packet = packets[i];
        const bool ofFirstFrame = i < 3;

        QCOMPARE(packet.header.ssrc, SSRC);
        QCOMPARE(packet.header.payloadType, PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, ofFirstFrame ? 90000u : 93000u);
        // Both numberings run on through the wrap, and the transport-wide one counts every packet.
        QCOMPARE(packet.header.sequence, static_cast<uint16_t>(FIRST.sequence + i));
        QCOMPARE(packet.transportSequence(), static_cast<uint16_t>(FIRST.transportSequence + i));
        QCOMPARE(packet.header.marker, i == 2 || i == 4);

        // The cipher leaves the fixed header and the extension preamble readable.
        QVERIFY(packet.header.extension);
        QCOMPARE(packet.clearSize, VideoSendStream::CLEAR_SIZE);
        QCOMPARE(packet.extensions.size(), size_t(VideoSendStream::EXTENSIONS_SIZE));
        QCOMPARE(packet.extension(VideoSendStream::TRANSMISSION_OFFSET_EXTENSION), (Bytes{ 0, 0, 0 }));
        QCOMPARE(packet.extension(VideoSendStream::ABS_SEND_TIME_EXTENSION).size(), size_t(3));
        QCOMPARE(packet.extension(VideoSendStream::TRANSPORT_SEQUENCE_EXTENSION).size(), size_t(2));
        // Nothing but those three: no MID, no RID.
        for (uint8_t other : { 1, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14 })
            QVERIFY(packet.extension(other).empty());
    }

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::VP8, payloadsOf({ packets.begin(), packets.begin() + 3 }), reassembled));
    QCOMPARE(reassembled, first);
    QVERIFY(reassemble(VideoCodec::VP8, payloadsOf({ packets.begin() + 3, packets.end() }), reassembled));
    QCOMPARE(reassembled, second);

    QCOMPARE(sender.stats().frames, quint64(2));
    QCOMPARE(sender.stats().keyframes, quint64(1));
    QCOMPARE(sender.stats().packets, quint64(5));
    QCOMPARE(sender.stats().bytes, quint64(5000 + 5 * VP8_DESCRIPTOR));
}

void TestVideoSend::theSendTimeIsSecondsInFixedPoint()
{
    VideoSendStream sender = vp8Sender();
    const auto sendTimeAt = [&sender](qint64 nowUs) {
        sender.queueFrame(vp8Frame(false, 10, 62), 0, false, nowUs);
        const std::vector<Written> packets = drain(sender, nowUs);
        return packets.size() == 1 ? packets[0].sendTime() : 0xFFFFFFFF;
    };

    // Eighteen bits of fraction: a second and a half is 1.5 * 2^18.
    QCOMPARE(sendTimeAt(1500 * MS), 0x060000u);
    QCOMPARE(sendTimeAt(250 * MS), 0x010000u);
    // Only six bits of whole seconds travel, so the value wraps every 64 seconds.
    QCOMPARE(sendTimeAt(65 * SECOND), 0x040000u);
    QCOMPARE(sendTimeAt(64 * SECOND - 4), 0xFFFFFFu);
}

void TestVideoSend::noPacketIsLargerThanTheLimit_data()
{
    QTest::addColumn<int>("codec");
    QTest::addColumn<Bytes>("frame");
    QTest::addColumn<int>("largest");

    Bytes unit;
    appendNal(unit, NAL_SLICE, MAX_PAYLOAD, 70);
    Bytes unitOver;
    appendNal(unitOver, NAL_SLICE, MAX_PAYLOAD + 1, 71);

    // A packet that is as large as a first transmission may be still has room to be repeated.
    constexpr int FULL = PACKET_OVERHEAD + MAX_PAYLOAD;
    QTest::newRow("VP8, exactly one packet") << int(VideoCodec::VP8) << vp8Frame(true, VP8_ROOM, 72) << FULL;
    QTest::newRow("VP8, one byte over") << int(VideoCodec::VP8) << vp8Frame(true, VP8_ROOM + 1, 73) << PACKET_OVERHEAD + VP8_DESCRIPTOR + VP8_ROOM / 2 + 1;
    QTest::newRow("VP8, many full packets") << int(VideoCodec::VP8) << vp8Frame(true, 172 * VP8_ROOM, 74) << FULL;
    QTest::newRow("H264, exactly one packet") << int(VideoCodec::H264) << unit << FULL;
    QTest::newRow("H264, one byte over") << int(VideoCodec::H264) << unitOver << PACKET_OVERHEAD + FU_HEADER + MAX_PAYLOAD / 2;
    QTest::newRow("H264, key frame of full packets") << int(VideoCodec::H264) << h264Keyframe(172 * (MAX_PAYLOAD - FU_HEADER) + 1) << FULL;
}

void TestVideoSend::noPacketIsLargerThanTheLimit()
{
    QFETCH(int, codec);
    QFETCH(Bytes, frame);
    QFETCH(int, largest);

    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(static_cast<VideoCodec>(codec), PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    QVERIFY(sender.queueFrame(frame, 0, true, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QVERIFY(!packets.empty());

    int largestSeen = 0;
    std::vector<uint16_t> everySequence;
    for (const Written &packet : packets) {
        largestSeen = std::max(largestSeen, packet.size);
        everySequence.push_back(packet.header.sequence);
    }
    QCOMPARE(largestSeen, largest);
    QVERIFY(largestSeen <= VideoSendStream::MAX_PACKET_SIZE);

    // Sent again, each is two bytes longer and still within the limit.
    sender.retransmit(everySequence, 0);
    const std::vector<Written> repeated = drain(sender, 0);
    QCOMPARE(repeated.size(), packets.size());
    for (size_t i = 0; i < repeated.size(); i++) {
        QCOMPARE(repeated[i].size, packets[i].size + VideoSendStream::RTX_HEADER_SIZE);
        QVERIFY(repeated[i].size <= VideoSendStream::MAX_PACKET_SIZE);
    }

    Bytes reassembled;
    QVERIFY(reassemble(static_cast<VideoCodec>(codec), payloadsOf(packets), reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::aFrameLeavesInSharesAndInOrder()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 30 * VP8_ROOM, 80), 0, true, 0));

    std::vector<size_t> shares;
    std::vector<Written> packets;
    for (qint64 now = 0; sender.hasWaiting(); now += VideoSendStream::PACE_INTERVAL_MS * MS) {
        const std::vector<Written> share = interval(sender, now);
        shares.push_back(share.size());
        packets.insert(packets.end(), share.begin(), share.end());
        QVERIFY(shares.size() <= 30);
    }

    QCOMPARE(shares, (std::vector<size_t>{ 8, 8, 8, 6 }));
    for (size_t i = 0; i < packets.size(); i++)
        QCOMPARE(packets[i].header.sequence, static_cast<uint16_t>(FIRST.sequence + i));
    QVERIFY(packets.back().header.marker);
    // Each share is stamped with the time it left at.
    QVERIFY(packets[0].sendTime() != packets[8].sendTime());
    QCOMPARE(packets[8].sendTime(), packets[15].sendTime());
}

void TestVideoSend::aBacklogLeavesFaster()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 400 * VP8_ROOM, 81), 0, true, 0));

    // A twentieth of what waits, as long as that is more than the usual share.
    QCOMPARE(interval(sender, 0).size(), size_t(20));
    QCOMPARE(interval(sender, 0).size(), size_t(19));

    int intervals = 2;
    while (sender.hasWaiting()) {
        QVERIFY(interval(sender, 0).size() >= size_t(VideoSendStream::PACKETS_PER_INTERVAL) || !sender.hasWaiting());
        intervals++;
    }
    // A key frame of nearly half a megabyte is out within a quarter of a second.
    QVERIFY(intervals * VideoSendStream::PACE_INTERVAL_MS <= 250);
}

void TestVideoSend::nothingLeavesOutsideAnInterval()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 20 * VP8_ROOM, 82), 0, true, 0));

    PacketBuffer packet;
    QVERIFY(!sender.writeNext(0, packet));

    QCOMPARE(interval(sender, 0).size(), size_t(8));
    // The share is spent; more waits for the next interval.
    QVERIFY(sender.hasWaiting());
    QVERIFY(!sender.writeNext(0, packet));
}

void TestVideoSend::aLostPacketIsSentAgainOnTheRetransmissionStream()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 5 * VP8_ROOM, 90), 4500, true, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QCOMPARE(packets.size(), size_t(5));

    // Twice the same one in a single request is still one packet.
    const uint16_t lost[] = { packets[4].header.sequence, packets[1].header.sequence, packets[4].header.sequence };
    sender.retransmit(lost, 200 * MS);
    const std::vector<Written> repeated = drain(sender, 250 * MS);
    QCOMPARE(repeated.size(), size_t(2));

    const Written *originals[] = { &packets[4], &packets[1] };
    for (size_t i = 0; i < repeated.size(); i++) {
        const Written &packet = repeated[i];
        const Written &original = *originals[i];

        QCOMPARE(packet.header.ssrc, RTX_SSRC);
        QCOMPARE(packet.header.payloadType, RTX_PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, 4500u);
        QCOMPARE(packet.header.marker, original.header.marker);
        // Its own sequence numbers; the transport-wide ones carry on from the first transmissions.
        QCOMPARE(packet.header.sequence, static_cast<uint16_t>(FIRST.rtxSequence + i));
        QCOMPARE(packet.transportSequence(), static_cast<uint16_t>(FIRST.transportSequence + 5 + i));
        QVERIFY(packet.sendTime() != original.sendTime());

        // RFC 4588: the original sequence number, then the original payload.
        QCOMPARE(packet.payload.size(), original.payload.size() + 2);
        QCOMPARE(qFromBigEndian<uint16_t>(packet.payload.data()), original.header.sequence);
        QCOMPARE(Bytes(packet.payload.begin() + 2, packet.payload.end()), original.payload);
    }

    QCOMPARE(sender.stats().nacksReceived, quint64(3));
    QCOMPARE(sender.stats().retransmitted, quint64(2));
    // The sender report counts first transmissions only.
    QCOMPARE(sender.stats().packets, quint64(5));
}

void TestVideoSend::retransmissionsGoAheadOfNewPackets()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 2 * VP8_ROOM, 91), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    QCOMPARE(sent.size(), size_t(2));

    QVERIFY(sender.queueFrame(vp8Frame(false, 12 * VP8_ROOM, 92), 3000, false, 10 * MS));
    const uint16_t lost[] = { sent[0].header.sequence };
    sender.retransmit(lost, 10 * MS);

    // The repeat takes the first place in the share, and counts against it.
    const std::vector<Written> share = interval(sender, 10 * MS);
    QCOMPARE(share.size(), size_t(VideoSendStream::PACKETS_PER_INTERVAL));
    QCOMPARE(share[0].header.ssrc, RTX_SSRC);
    for (size_t i = 1; i < share.size(); i++) {
        QCOMPARE(share[i].header.ssrc, SSRC);
        QCOMPARE(share[i].header.sequence, static_cast<uint16_t>(sent[1].header.sequence + i));
    }
}

void TestVideoSend::whatWasNeverSentIsNotSentAgain()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 3 * VP8_ROOM, 93), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    QCOMPARE(sent.size(), size_t(3));

    const uint16_t unknown[] = { static_cast<uint16_t>(sent[0].header.sequence - 1), static_cast<uint16_t>(sent[2].header.sequence + 1), 12345 };
    sender.retransmit(unknown, MS);
    QVERIFY(!sender.hasWaiting());
    QVERIFY(drain(sender, MS).empty());
}

void TestVideoSend::theHistoryEndsAfterASecond()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 2 * VP8_ROOM, 94), 0, true, 0));
    const std::vector<Written> early = drain(sender, 0);
    QVERIFY(sender.queueFrame(vp8Frame(false, VP8_ROOM, 95), 3000, false, 600 * MS));
    const std::vector<Written> late = drain(sender, 600 * MS);
    QCOMPARE(early.size() + late.size(), size_t(3));

    const uint16_t first[] = { early[0].header.sequence };
    const uint16_t second[] = { early[1].header.sequence };
    const uint16_t third[] = { late[0].header.sequence };

    // A second old: still there.
    sender.retransmit(first, 1000 * MS);
    QCOMPARE(drain(sender, 1000 * MS).size(), size_t(1));

    // Asked for in time but not out before the second is over.
    sender.retransmit(second, 1000 * MS);
    QVERIFY(drain(sender, 1000 * MS + 1).empty());

    sender.retransmit(first, 1001 * MS);
    QVERIFY(!sender.hasWaiting());

    // What was sent later is kept its own second.
    sender.retransmit(third, 1600 * MS);
    const std::vector<Written> repeated = drain(sender, 1600 * MS);
    QCOMPARE(repeated.size(), size_t(1));
    QCOMPARE(qFromBigEndian<uint16_t>(repeated[0].payload.data()), late[0].header.sequence);
    sender.retransmit(third, 1600 * MS + 1);
    QVERIFY(!sender.hasWaiting());
}

void TestVideoSend::withoutARetransmissionPayloadTypeNothingIsSentAgain()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(VideoCodec::VP8, PAYLOAD_TYPE, std::nullopt);

    // With nothing to leave room for, a packet may be filled to the limit.
    QVERIFY(sender.queueFrame(vp8Frame(true, VP8_ROOM + 2, 96), 0, true, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QCOMPARE(packets.size(), size_t(1));
    QCOMPARE(packets[0].size, VideoSendStream::MAX_PACKET_SIZE);

    const uint16_t lost[] = { packets[0].header.sequence };
    sender.retransmit(lost, MS);
    QVERIFY(!sender.hasWaiting());
}

void TestVideoSend::aReceiverRepairsALossFromTheHistory()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(VideoCodec::H264, PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    VideoReceiver receiver(SSRC, VideoCodec::H264);
    PacketPool pool;

    const auto deliver = [&](const Written &packet, qint64 nowMs) {
        const bool retransmission = packet.header.ssrc == RTX_SSRC;
        PacketPool::Handle payload = pool.acquire();
        payload->append(std::span(packet.payload).subspan(retransmission ? 2 : 0));
        const uint16_t sequence = retransmission ? qFromBigEndian<uint16_t>(packet.payload.data()) : packet.header.sequence;
        receiver.insert(sequence, packet.header.timestamp, packet.header.marker, std::move(payload), nowMs, retransmission);
    };

    const Bytes keyframe = h264Keyframe(6000);
    Bytes delta;
    appendNal(delta, NAL_SLICE, 4000, 97);
    QVERIFY(sender.queueFrame(keyframe, 0, true, 0));
    QVERIFY(sender.queueFrame(delta, 3000, false, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QVERIFY(packets.size() > 6);

    constexpr size_t LOST = 3;
    for (size_t i = 0; i < packets.size(); i++) {
        if (i != LOST)
            deliver(packets[i], 0);
    }
    QVERIFY(receiver.takeFrames().empty());

    const VideoReceiver::Feedback feedback = receiver.poll(VideoReceiver::REORDER_GRACE_MS);
    QCOMPARE(feedback.nacks, std::vector<uint16_t>{ packets[LOST].header.sequence });

    sender.retransmit(feedback.nacks, 20 * MS);
    const std::vector<Written> repeated = drain(sender, 20 * MS);
    QCOMPARE(repeated.size(), size_t(1));
    deliver(repeated[0], 20);

    const std::vector<EncodedVideoFrame> frames = receiver.takeFrames();
    QCOMPARE(frames.size(), size_t(2));
    QVERIFY(frames[0].keyframe);
    QCOMPARE(frames[0].data, keyframe);
    QVERIFY(!frames[1].keyframe);
    QCOMPARE(frames[1].data, delta);
    QCOMPARE(frames[1].rtpTimestamp, 3000u);
}

void TestVideoSend::changingTheCodecKeepsTheNumbering()
{
    VideoSendStream sender = vp8Sender();
    QCOMPARE(sender.codec(), VideoCodec::VP8);
    QVERIFY(sender.queueFrame(vp8Frame(true, 100, 100), 0, true, 0));
    const std::vector<Written> before = drain(sender, 0);
    QCOMPARE(before.size(), size_t(1));

    // What still waits belongs to the old codec and is dropped with it.
    QVERIFY(sender.queueFrame(vp8Frame(false, 100, 101), 3000, false, 0));
    constexpr uint8_t H264_PAYLOAD_TYPE = 103;
    constexpr uint8_t H264_RTX_PAYLOAD_TYPE = 104;
    sender.setCodec(VideoCodec::H264, H264_PAYLOAD_TYPE, H264_RTX_PAYLOAD_TYPE);
    QCOMPARE(sender.codec(), VideoCodec::H264);
    QVERIFY(!sender.hasWaiting());

    const Bytes frame = h264Keyframe(100);
    QVERIFY(sender.queueFrame(frame, 6000, true, 0));
    const std::vector<Written> after = drain(sender, 0);
    QCOMPARE(after.size(), size_t(1));
    QCOMPARE(after[0].header.payloadType, H264_PAYLOAD_TYPE);
    QCOMPARE(after[0].header.sequence, static_cast<uint16_t>(before[0].header.sequence + 1));
    QCOMPARE(after[0].transportSequence(), static_cast<uint16_t>(before[0].transportSequence() + 1));

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloadsOf(after), reassembled));
    QCOMPARE(reassembled, frame);

    // A packet of the old codec cannot be repeated under the new one's payload type.
    const uint16_t old[] = { before[0].header.sequence };
    sender.retransmit(old, 0);
    QVERIFY(!sender.hasWaiting());

    // Naming the same codec again changes nothing.
    QVERIFY(sender.queueFrame(frame, 9000, true, 0));
    sender.setCodec(VideoCodec::H264, H264_PAYLOAD_TYPE, H264_RTX_PAYLOAD_TYPE);
    QVERIFY(sender.hasWaiting());
}

void TestVideoSend::clearingForgetsWhatWaited()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 20 * VP8_ROOM, 102), 0, true, 0));
    const std::vector<Written> sent = interval(sender, 0);
    QCOMPARE(sent.size(), size_t(8));

    sender.clear();
    QVERIFY(!sender.hasWaiting());
    QVERIFY(interval(sender, 0).empty());
    const uint16_t lost[] = { sent[0].header.sequence };
    sender.retransmit(lost, 0);
    QVERIFY(!sender.hasWaiting());

    // What goes out next follows on from the last packet that did go out.
    QVERIFY(sender.queueFrame(vp8Frame(true, 10, 103), 3000, true, 0));
    const std::vector<Written> next = drain(sender, 0);
    QCOMPARE(next.size(), size_t(1));
    QCOMPARE(next[0].header.sequence, static_cast<uint16_t>(sent[7].header.sequence + 1));
}

void TestVideoSend::framesAreRefusedWithoutACodec()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    QCOMPARE(sender.codec(), VideoCodec::Unknown);
    QVERIFY(!sender.queueFrame(vp8Frame(true, 100, 104), 0, true, 0));

    sender.setCodec(VideoCodec::AV1, PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    QCOMPARE(sender.codec(), VideoCodec::Unknown);
    QVERIFY(!sender.queueFrame(vp8Frame(true, 100, 104), 0, true, 0));
    QVERIFY(!sender.hasWaiting());
    QVERIFY(!sender.canReport());
    QCOMPARE(sender.stats().frames, quint64(0));
}

void TestVideoSend::theSenderReportStatesWhatWasSent()
{
    VideoSendStream sender = vp8Sender();
    uint8_t buffer[64];
    // There is no RTP time to state before the first frame.
    QVERIFY(!sender.canReport());
    QVERIFY(sender.writeSenderReport(buffer, 0, 0) < 0);

    QVERIFY(sender.queueFrame(vp8Frame(true, 3000, 110), 90000, true, 1 * SECOND));
    const std::vector<Written> packets = drain(sender, 1 * SECOND);
    const uint16_t lost[] = { packets[0].header.sequence };
    sender.retransmit(lost, 1 * SECOND);
    QCOMPARE(drain(sender, 1 * SECOND).size(), size_t(1));

    // 2026-01-01 00:00:00.75 UTC in seconds since 1900.
    constexpr uint64_t NTP_TIME = (uint64_t(3976214400) << 32) | 0xC0000000u;
    const int size = sender.writeSenderReport(buffer, 1500 * MS, NTP_TIME);
    QCOMPARE(size, 28);
    QCOMPARE(classifyPacket({ buffer, size_t(size) }), PacketKind::Rtcp);

    const ReceivedRtcp parsed = parseRtcp({ buffer, size_t(size) });
    QCOMPARE(parsed.senderReports.size(), size_t(1));
    QVERIFY(parsed.receptionReports.empty());
    const ReceivedRtcp::SenderReport &report = parsed.senderReports[0];
    QCOMPARE(report.ssrc, SSRC);
    QCOMPARE(report.compactNtp, uint32_t((3976214400u << 16) | 0xC000));
    // Half a second on from the frame's own time, at 90 kHz.
    QCOMPARE(report.rtpTimestamp, 90000u + 45000u);
    // Retransmissions are another stream's and are not counted in this one's report.
    QCOMPARE(report.packetCount, 3u);
    QCOMPARE(report.octetCount, uint32_t(3000 + 3 * VP8_DESCRIPTOR));
    QCOMPARE(qFromBigEndian<quint64>(buffer + 8), NTP_TIME);

    uint8_t tooSmall[27];
    QVERIFY(sender.writeSenderReport(tooSmall, 1500 * MS, NTP_TIME) < 0);
}

void TestVideoSend::aReceptionReportGivesLossAndRoundTrip()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.stats().lossFraction < 0);
    QCOMPARE(sender.stats().roundTripMs, -1);

    QVERIFY(sender.queueFrame(vp8Frame(true, 100, 111), 0, true, 0));
    uint8_t buffer[64];
    constexpr uint64_t FIRST_NTP = (uint64_t(3976214400) << 32);
    constexpr uint64_t SECOND_NTP = (uint64_t(3976214401) << 32);
    QVERIFY(sender.writeSenderReport(buffer, 1 * SECOND, FIRST_NTP) > 0);
    QVERIFY(sender.writeSenderReport(buffer, 2 * SECOND, SECOND_NTP) > 0);

    // The loss is taken from a report that answers no sender report, the round trip is not.
    ReceptionReport report;
    report.ssrc = SSRC;
    report.fractionLost = 64;
    sender.onReceptionReport(report, 2100 * MS);
    QCOMPARE(sender.stats().lossFraction, 0.25);
    QCOMPARE(sender.stats().roundTripMs, -1);

    // The first report came back 400 ms after it left, of which the receiver held it 250.
    report.fractionLost = 0;
    report.lastSenderReport = compactNtp(FIRST_NTP);
    report.delaySinceLastSenderReport = 65536 / 4;
    sender.onReceptionReport(report, 1400 * MS);
    QCOMPARE(sender.stats().lossFraction, 0.0);
    QCOMPARE(sender.stats().roundTripMs, 150);

    report.lastSenderReport = compactNtp(SECOND_NTP);
    report.delaySinceLastSenderReport = 0;
    sender.onReceptionReport(report, 2030 * MS);
    QCOMPARE(sender.stats().roundTripMs, 30);

    // An echo of something this sender never sent says nothing.
    report.lastSenderReport = 0x12345678;
    sender.onReceptionReport(report, 5 * SECOND);
    QCOMPARE(sender.stats().roundTripMs, 30);
}

void TestVideoSend::feedbackForTheSenderIsRead()
{
    constexpr uint32_t SERVER = 0xAABBCCDD;
    constexpr uint32_t OTHER = 0x4321;

    // A receiver report with one block about the video, as a compound packet opens.
    Bytes block;
    append32(block, SERVER);
    append32(block, SSRC);
    block.push_back(32);
    block.insert(block.end(), { 0, 0, 9 });
    append32(block, 70000);
    append32(block, 11);
    append32(block, 0x11223344);
    append32(block, 6554);
    Bytes compound = rtcpPacket(1, 201, block);

    // Generic NACK: packet 0xFFFE and, by the bitmask, the first and the third after it.
    Bytes nack;
    append32(nack, SERVER);
    append32(nack, SSRC);
    append16(nack, 0xFFFE);
    append16(nack, 0x0005);
    append16(nack, 300);
    append16(nack, 0);
    const Bytes nackPacket = rtcpPacket(1, 205, nack);
    compound.insert(compound.end(), nackPacket.begin(), nackPacket.end());

    // Transport-wide feedback about this client's own packets is taken without complaint.
    Bytes transportWide;
    append32(transportWide, SERVER);
    append32(transportWide, SSRC);
    append16(transportWide, 1);
    append16(transportWide, 1);
    append32(transportWide, 0x00000100);
    append16(transportWide, 0x2001);
    append16(transportWide, 0x0400);
    const Bytes transportWidePacket = rtcpPacket(15, 205, transportWide);
    compound.insert(compound.end(), transportWidePacket.begin(), transportWidePacket.end());

    Bytes pictureLoss;
    append32(pictureLoss, SERVER);
    append32(pictureLoss, SSRC);
    const Bytes pictureLossPacket = rtcpPacket(1, 206, pictureLoss);
    compound.insert(compound.end(), pictureLossPacket.begin(), pictureLossPacket.end());

    // A full intra request leaves the media source zero and names its streams in entries.
    Bytes fullIntra;
    append32(fullIntra, SERVER);
    append32(fullIntra, 0);
    append32(fullIntra, OTHER);
    fullIntra.insert(fullIntra.end(), { 7, 0, 0, 0 });
    append32(fullIntra, SSRC);
    fullIntra.insert(fullIntra.end(), { 8, 0, 0, 0 });
    const Bytes fullIntraPacket = rtcpPacket(4, 206, fullIntra);
    compound.insert(compound.end(), fullIntraPacket.begin(), fullIntraPacket.end());

    const ReceivedRtcp parsed = parseRtcp(compound);

    QCOMPARE(parsed.receptionReports.size(), size_t(1));
    QCOMPARE(parsed.receptionReports[0].ssrc, SSRC);
    QCOMPARE(parsed.receptionReports[0].fractionLost, uint8_t(32));
    QCOMPARE(parsed.receptionReports[0].cumulativeLost, 9);
    QCOMPARE(parsed.receptionReports[0].lastSenderReport, 0x11223344u);
    QCOMPARE(parsed.receptionReports[0].delaySinceLastSenderReport, 6554u);

    QCOMPARE(parsed.nacks.size(), size_t(1));
    QCOMPARE(parsed.nacks[0].mediaSsrc, SSRC);
    QCOMPARE(parsed.nacks[0].sequences, (std::vector<uint16_t>{ 0xFFFE, 0xFFFF, 1, 300 }));

    QCOMPARE(parsed.keyframeRequests, (std::vector<uint32_t>{ SSRC, OTHER, SSRC }));
    QVERIFY(parsed.receiverCounts.empty());
    QVERIFY(parsed.senderReports.empty());
    QVERIFY(!parsed.hasInboundEstimate);

    // What this client writes for the streams it receives reads back the same way.
    uint8_t written[64];
    const uint16_t missing[] = { 10, 11, 40 };
    const int nackSize = writeNack(written, SERVER, OTHER, missing);
    QVERIFY(nackSize > 0);
    const int pliSize = writePli(std::span(written).subspan(static_cast<size_t>(nackSize)), SERVER, OTHER);
    QVERIFY(pliSize > 0);
    const ReceivedRtcp own = parseRtcp({ written, static_cast<size_t>(nackSize + pliSize) });
    QCOMPARE(own.nacks.size(), size_t(1));
    QCOMPARE(own.nacks[0].mediaSsrc, OTHER);
    QCOMPARE(own.nacks[0].sequences, (std::vector<uint16_t>{ 10, 11, 40 }));
    QCOMPARE(own.keyframeRequests, std::vector<uint32_t>{ OTHER });
}

void TestVideoSend::theReceiverCountIsRead()
{
    constexpr uint32_t SERVER = 0xAABBCCDD;
    const auto receiverCount = [](uint32_t ssrc, int receivers) {
        Bytes body;
        append32(body, ssrc);
        body.insert(body.end(), { 0x30, 0x53, 0x49, 0x44 });
        for (int i = 0; i < receivers; i++) {
            // A user id, that user's bitrate, and one more byte.
            append32(body, 0x00112233);
            append32(body, 0x44556600u + static_cast<uint32_t>(i));
            body.insert(body.end(), { 0x12, 0x34, 0x56, 0x00 });
        }
        return rtcpPacket(static_cast<uint8_t>(receivers), 204, body);
    };

    Bytes compound = rtcpPacket(0, 201, { 0xAA, 0xBB, 0xCC, 0xDD });
    for (const Bytes &packet : { receiverCount(SSRC, 3), receiverCount(0x4321, 0) })
        compound.insert(compound.end(), packet.begin(), packet.end());

    // Its sibling, the server's estimate of the way here, in the same compound packet.
    Bytes estimate;
    append32(estimate, SERVER);
    estimate.insert(estimate.end(), { 0x31, 0x53, 0x49, 0x44 });
    append32(estimate, 0);
    append32(estimate, 3);
    // 100000 under an exponent of one, and a quarter lost.
    estimate.insert(estimate.end(), { 0x05, 0x86, 0xA0, 64 });
    const Bytes estimatePacket = rtcpPacket(0, 204, estimate);
    compound.insert(compound.end(), estimatePacket.begin(), estimatePacket.end());

    const ReceivedRtcp parsed = parseRtcp(compound);
    QCOMPARE(parsed.receiverCounts.size(), size_t(2));
    QCOMPARE(parsed.receiverCounts[0].ssrc, SSRC);
    QCOMPARE(parsed.receiverCounts[0].receivers, 3);
    QCOMPARE(parsed.receiverCounts[1].ssrc, 0x4321u);
    QCOMPARE(parsed.receiverCounts[1].receivers, 0);
    QVERIFY(parsed.hasInboundEstimate);
    QCOMPARE(parsed.inboundBitsPerSecond, quint64(100000 << 1));
    QCOMPARE(parsed.inboundLoss, 0.25);

    // An application packet under another name is neither of the two.
    Bytes other;
    append32(other, SSRC);
    other.insert(other.end(), { 'D', 'I', 'S', '0' });
    QVERIFY(parseRtcp(rtcpPacket(5, 204, other)).receiverCounts.empty());
}

QTEST_GUILESS_MAIN(TestVideoSend)
#include "tst_VideoSend.moc"
