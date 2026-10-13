// The estimate of what the way to the server carries, made from the server's reports of when
// each packet reached it. The way is one of the test's own: a queue that lets so many bits a
// second through, with the server's reports written by the same code this client reports to
// the server with.

#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/SendSideEstimator.hpp"
#include "Discord/Voice/TransportFeedback.hpp"

#include <QTest>
#include <QtEndian>

#include <deque>
#include <functional>
#include <vector>

using namespace Acheron::Discord::Voice;

namespace {

constexpr int PACKET_BYTES = 1200;
constexpr qint64 REPORT_EVERY_US = 100'000;
constexpr uint32_t SERVER_SSRC = 0xAABBCCDD;
constexpr uint32_t VIDEO_SSRC = 0x11223344;

// A queue that lets `capacity` bits a second through, and takes `delayUs` on top.
struct Way
{
    double capacity = 0;
    qint64 delayUs = 20'000;
    qint64 freeAtUs = 0;

    qint64 arrivalOf(qint64 sentUs, int bytes)
    {
        const qint64 startedUs = std::max(sentUs, freeAtUs);
        freeAtUs = startedUs + static_cast<qint64>(bytes * 8e6 / capacity);
        return freeAtUs + delayUs;
    }
};

// A sender of evenly spaced packets, the way between it and the server, and the server's
// reports every tenth of a second.
class Call
{
public:
    explicit Call(double capacity, uint16_t firstSequence = 1)
    {
        way.capacity = capacity;
        estimator.restart(firstSequence);
    }

    // Sends for `seconds` at what `rate` gives each time it is asked, in bits per second.
    void run(double seconds, const std::function<double()> &rate)
    {
        const qint64 untilUs = nowUs + static_cast<qint64>(seconds * 1e6);
        while (nowUs < untilUs) {
            if (nowUs >= nextPacketUs) {
                const uint16_t number = estimator.numberPacket(nowUs, PACKET_BYTES);
                onTheWay.push_back({ way.arrivalOf(nowUs, PACKET_BYTES), number });
                nextPacketUs = nowUs + static_cast<qint64>(PACKET_BYTES * 8e6 / rate());
            }
            if (nowUs >= nextReportUs) {
                report();
                nextReportUs = nowUs + REPORT_EVERY_US;
            }
            nowUs += 500;
        }
    }

    void run(double seconds, double rate)
    {
        run(seconds, [rate] { return rate; });
    }

    SendSideEstimator estimator;
    Way way;
    quint64 changes = 0;

private:
    struct OnTheWay
    {
        qint64 arrivalUs = 0;
        uint16_t number = 0;
    };

    void report()
    {
        while (!onTheWay.empty() && onTheWay.front().arrivalUs <= nowUs) {
            server.onPacket(onTheWay.front().number, onTheWay.front().arrivalUs);
            onTheWay.pop_front();
        }
        while (server.hasPending()) {
            std::vector<uint8_t> packet(1400);
            const int size = server.write(packet, SERVER_SSRC, VIDEO_SSRC);
            if (size <= 0)
                break;
            packet.resize(static_cast<size_t>(size));
            for (const ReceivedRtcp::TransportReport &read : parseRtcp(packet).transportReports)
                changes += estimator.onReport(read, nowUs) ? 1 : 0;
        }
    }

    TransportFeedback server;
    std::deque<OnTheWay> onTheWay;
    qint64 nowUs = 1'000'000;
    qint64 nextPacketUs = 0;
    qint64 nextReportUs = 0;
};

} // namespace

class TestSendSideEstimator : public QObject
{
    Q_OBJECT
private slots:
    void aReportIsReadAsItWasWritten();
    void aReportCutShortIsNotRead();
    void nothingIsKnownOfAWayThatKeepsUp();
    void aWayThatFallsBehindIsBackedOffFrom();
    void itClimbsBackOnceTheWayKeepsUp();
    void theNumberingRunsThroughItsWrap();
    void aReportOnPacketsNotSentIsPassedOver();
};

void TestSendSideEstimator::aReportIsReadAsItWasWritten()
{
    // Packets 10 to 15 with 12 missing, one of them arriving before the one numbered ahead
    // of it, and one a long while after the rest.
    TransportFeedback server;
    server.onPacket(10, 5'000'000);
    server.onPacket(11, 5'001'250);
    server.onPacket(13, 5'003'000);
    server.onPacket(14, 5'002'500);
    server.onPacket(15, 5'090'000);

    std::vector<uint8_t> packet(200);
    const int size = server.write(packet, SERVER_SSRC, VIDEO_SSRC);
    QVERIFY(size > 0);
    packet.resize(static_cast<size_t>(size));

    const ReceivedRtcp received = parseRtcp(packet);
    QCOMPARE(received.transportReports.size(), size_t(1));
    const ReceivedRtcp::TransportReport &report = received.transportReports.front();
    QCOMPARE(report.mediaSsrc, VIDEO_SSRC);
    QCOMPARE(report.baseSequence, uint16_t(10));
    QCOMPARE(report.referenceTime, 5'000'000 / 64'000);
    QCOMPARE(report.packets.size(), size_t(6));

    // In quarter milliseconds from the reference time, which is 8 ms before the first.
    const std::vector<std::pair<bool, int>> expected = { { true, 32 }, { true, 5 }, { false, 0 }, { true, 7 }, { true, -2 }, { true, 350 } };
    for (size_t i = 0; i < expected.size(); i++) {
        QCOMPARE(report.packets[i].received, expected[i].first);
        QCOMPARE(report.packets[i].delta, expected[i].second);
    }
}

void TestSendSideEstimator::aReportCutShortIsNotRead()
{
    TransportFeedback server;
    for (uint16_t number = 0; number < 40; number++)
        server.onPacket(number, 1'000'000 + number * 3000);
    std::vector<uint8_t> packet(400);
    const int size = server.write(packet, SERVER_SSRC, VIDEO_SSRC);
    QVERIFY(size > 0);
    packet.resize(static_cast<size_t>(size));
    QCOMPARE(parseRtcp(packet).transportReports.size(), size_t(1));

    // The length in the header shortened by a word at a time: nothing of it can be read
    // once deltas are missing, and nothing is read past the end.
    for (int words = 1; size - 4 * words >= 20; words++) {
        std::vector<uint8_t> shorter(packet.begin(), packet.end() - 4 * words);
        qToBigEndian(static_cast<uint16_t>(shorter.size() / 4 - 1), shorter.data() + 2);
        shorter[0] &= ~0x20;
        QVERIFY(parseRtcp(shorter).transportReports.empty());
    }
}

void TestSendSideEstimator::nothingIsKnownOfAWayThatKeepsUp()
{
    Call call(5'000'000);
    call.run(20, 2'000'000);

    QCOMPARE(call.estimator.estimate(), qint64(0));
    QVERIFY(!call.estimator.overusing());
    QVERIFY(call.estimator.reportsTaken() > 150);
    const qint64 acknowledged = call.estimator.acknowledged();
    QVERIFY2(acknowledged > 1'800'000 && acknowledged < 2'200'000, qPrintable(QString::number(acknowledged)));
}

void TestSendSideEstimator::aWayThatFallsBehindIsBackedOffFrom()
{
    // Twice what the way carries: every packet waits longer than the one before.
    Call call(1'000'000);
    call.run(3, 2'000'000);

    const qint64 estimate = call.estimator.estimate();
    QVERIFY2(estimate > 700'000 && estimate < 950'000, qPrintable(QString::number(estimate)));
    const qint64 acknowledged = call.estimator.acknowledged();
    QVERIFY2(acknowledged > 900'000 && acknowledged < 1'100'000, qPrintable(QString::number(acknowledged)));
    QVERIFY(call.changes > 0);
}

void TestSendSideEstimator::itClimbsBackOnceTheWayKeepsUp()
{
    Call call(1'000'000);
    call.run(3, 2'000'000);
    const qint64 backedOff = call.estimator.estimate();
    QVERIFY(backedOff > 0);

    // The way widens, and the sender goes by the estimate: it creeps while it is near
    // where the way gave out, then climbs by a share of itself, never far above what is
    // seen to arrive.
    call.way.capacity = 6'000'000;
    call.run(5, [&call] { return static_cast<double>(call.estimator.estimate()); });
    const qint64 soonAfter = call.estimator.estimate();
    QVERIFY2(soonAfter > backedOff && soonAfter < backedOff * 1.6, qPrintable(QStringLiteral("%1 after %2").arg(soonAfter).arg(backedOff)));

    call.run(40, [&call] { return static_cast<double>(call.estimator.estimate()); });
    const qint64 later = call.estimator.estimate();
    QVERIFY2(later > backedOff * 3 && later < 6'000'000, qPrintable(QStringLiteral("%1 after %2").arg(later).arg(backedOff)));
    QVERIFY(later <= call.estimator.acknowledged() * 1.5 + 10'000 + 1);
}

void TestSendSideEstimator::theNumberingRunsThroughItsWrap()
{
    // 2 Mbps is some two hundred packets a second, so the numbering wraps within three.
    Call call(6'000'000, 65'000);
    call.run(4, 2'000'000);
    const qint64 acknowledged = call.estimator.acknowledged();
    QVERIFY2(acknowledged > 1'800'000 && acknowledged < 2'200'000, qPrintable(QString::number(acknowledged)));

    // What is acknowledged from here on is all of packets numbered after the wrap.
    call.run(4, 3'000'000);
    const qint64 afterTheWrap = call.estimator.acknowledged();
    QVERIFY2(afterTheWrap > 2'700'000 && afterTheWrap < 3'300'000, qPrintable(QString::number(afterTheWrap)));
    QCOMPARE(call.estimator.estimate(), qint64(0));
}

void TestSendSideEstimator::aReportOnPacketsNotSentIsPassedOver()
{
    ReceivedRtcp::TransportReport report;
    report.baseSequence = 500;
    report.packets.assign(20, { true, 4 });

    SendSideEstimator estimator;
    estimator.restart(1);
    QVERIFY(!estimator.onReport(report, 1'000'000));
    QCOMPARE(estimator.reportsTaken(), quint64(0));

    for (int i = 0; i < 10; i++)
        estimator.numberPacket(1'000'000 + i * 1000, PACKET_BYTES);
    QVERIFY(!estimator.onReport(report, 2'000'000));
    QCOMPARE(estimator.acknowledged(), qint64(0));
    QCOMPARE(estimator.estimate(), qint64(0));
}

QTEST_GUILESS_MAIN(TestSendSideEstimator)
#include "tst_SendSideEstimator.moc"
