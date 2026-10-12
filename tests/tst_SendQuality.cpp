// Tests for what a screen share is sent at: which qualities an account may pick, the size and
// bitrates announced for each, how the bitrates follow the size viewers show the stream at, and
// the controller that moves the encoder's bitrate by reported loss. The quality rules are the
// official client's (docs/video-re/re-voice-gateway-video.md).

#include "Core/Video/SendQuality.hpp"
#include "Core/Video/SendRateController.hpp"

#include <QTest>

using namespace Acheron::Core::Video;

// allowed[premium_type][premium_tier]
using TierTable = std::array<std::array<bool, 4>, 4>;
Q_DECLARE_METATYPE(TierTable)

namespace {

constexpr int SOURCE = SendQuality::SOURCE_HEIGHT;
constexpr qint64 PIXELS_720P = 1280 * 720;

const BitrateLimits FULL_720P{ 500'000, 600'000, 3'500'000 };
const BitrateLimits FULL_1080P{ 500'000, 600'000, 9'000'000 };

constexpr TierTable EVERYONE{ { { true, true, true, true }, { true, true, true, true }, { true, true, true, true }, { true, true, true, true } } };
constexpr TierTable BOOST_ONE_OR_NITRO{ { { false, true, true, true }, { true, true, true, true }, { true, true, true, true }, { false, true, true, true } } };
constexpr TierTable BOOST_TWO_OR_NITRO{ { { false, false, true, true }, { true, true, true, true }, { true, true, true, true }, { false, false, true, true } } };
constexpr TierTable FULL_NITRO_ONLY{ { { false, false, false, false }, { false, false, false, false }, { true, true, true, true }, { false, false, false, false } } };

} // namespace

class TestSendQuality : public QObject
{
    Q_OBJECT
private slots:
    void qualityIsGatedByNitroAndBoosts_data();
    void qualityIsGatedByNitroAndBoosts();
    void unlistedHeightsAndFrameratesAreRefused();
    void aQualityTheAccountMayNotUseFallsBackToTheDefault();
    void movingToAnotherHeightKeepsTheFramerateWhereAllowed();

    void announcedSizeIsFourByThreeAt480AndSixteenByNineAbove();
    void maxBitrateFollowsHeightAndFramerate_data();
    void maxBitrateFollowsHeightAndFramerate();

    void bitratesAreNotScaledWithoutASmallerViewer();
    void sourceQualityIsNeverScaled();
    void bitratesScaleWithTheViewersShareOfThePicture();
    void scaledBitratesAreRoundedUp();
    void scaledMinimumAndTargetStopAtTheFloor();
    void scaledMaximumKeepsRoomForThePicture();
    void scaledMaximumNeverPassesTheFullMaximum();

    void firstViewerSizeIsTakenUp();
    void viewerSizeWithinFivePercentIsIgnored();
    void viewerSizeIsComparedWithTheOneInUse();
    void viewersGoingAwayClearsTheSize();

    void controllerStartsAtTheMaximum();
    void controllerIgnoresReportsBeforeTheServerHasSaid();
    void heavyLossLowersTheBitrate();
    void heavyLossLowersNoMoreOftenThanEveryTwoSeconds();
    void bitrateNeverGoesUnderTheMinimum();
    void moderateLossHolds();
    void tenSecondsWithoutLossRaisesTheBitrate();
    void eachRaiseNeedsAnotherTenCleanSeconds();
    void raisesStopAtTheMaximum();
    void lossInterruptsTheCleanStretch();
    void newLimitsKeepTheShareOfTheMaximum();
    void aMaximumUnderTheMinimumIsStillAMaximum();
    void restartGoesBackToTheMaximum();
};

void TestSendQuality::qualityIsGatedByNitroAndBoosts_data()
{
    QTest::addColumn<int>("height");
    QTest::addColumn<int>("framerate");
    QTest::addColumn<TierTable>("allowed");

    for (const int height : { 480, 720 }) {
        QTest::addRow("%dp15", height) << height << 15 << EVERYONE;
        QTest::addRow("%dp30", height) << height << 30 << EVERYONE;
        QTest::addRow("%dp60", height) << height << 60 << BOOST_ONE_OR_NITRO;
    }
    for (const int height : { 1080, 1440 }) {
        for (const int framerate : { 15, 30, 60 })
            QTest::addRow("%dp%d", height, framerate) << height << framerate << BOOST_TWO_OR_NITRO;
    }
    for (const int framerate : { 15, 30, 60 })
        QTest::addRow("source at %d", framerate) << SOURCE << framerate << FULL_NITRO_ONLY;
}

void TestSendQuality::qualityIsGatedByNitroAndBoosts()
{
    QFETCH(int, height);
    QFETCH(int, framerate);
    QFETCH(TierTable, allowed);

    for (int premiumType = 0; premiumType < 4; premiumType++) {
        for (int boostTier = 0; boostTier < 4; boostTier++) {
            const bool may = mayUseQuality({ height, framerate }, { premiumType, boostTier });
            QVERIFY2(may == allowed[premiumType][boostTier], qPrintable(QStringLiteral("premium type %1, boost tier %2").arg(premiumType).arg(boostTier)));
        }
    }
}

void TestSendQuality::unlistedHeightsAndFrameratesAreRefused()
{
    const SharerTier nitro{ 2, 3 };
    QVERIFY(!mayUseQuality({ 720, 5 }, nitro));
    QVERIFY(!mayUseQuality({ 720, 24 }, nitro));
    QVERIFY(!mayUseQuality({ 360, 30 }, nitro));
    QVERIFY(!mayUseQuality({ 2160, 30 }, nitro));
}

void TestSendQuality::aQualityTheAccountMayNotUseFallsBackToTheDefault()
{
    const SendQuality standard;
    QCOMPARE(standard.height, 720);
    QCOMPARE(standard.framerate, 30);

    QCOMPARE(usableQuality({ 1080, 60 }, {}), standard);
    QCOMPARE(usableQuality({ SOURCE, 30 }, { 1, 3 }), standard);
    QCOMPARE(usableQuality({ 1080, 60 }, { 1, 0 }), (SendQuality{ 1080, 60 }));
    QCOMPARE(usableQuality({ 480, 15 }, {}), (SendQuality{ 480, 15 }));
}

void TestSendQuality::movingToAnotherHeightKeepsTheFramerateWhereAllowed()
{
    const SharerTier boostedTwice{ 0, 2 };
    QCOMPARE(atHeight({ 720, 60 }, 1080, boostedTwice), (SendQuality{ 1080, 60 }));

    const SharerTier boostedOnce{ 0, 1 };
    QCOMPARE(atHeight({ 1080, 15 }, 720, boostedOnce), (SendQuality{ 720, 15 }));
    QCOMPARE(atHeight({ 720, 60 }, 480, boostedOnce), (SendQuality{ 480, 60 }));
    QCOMPARE(atHeight({ 720, 60 }, 480, {}), (SendQuality{ 480, 30 }));
}

void TestSendQuality::announcedSizeIsFourByThreeAt480AndSixteenByNineAbove()
{
    QCOMPARE(announcedSize({ 480, 30 }), QSize(640, 480));
    QCOMPARE(announcedSize({ 720, 30 }), QSize(1280, 720));
    QCOMPARE(announcedSize({ 1080, 60 }), QSize(1920, 1080));
    QCOMPARE(announcedSize({ 1440, 15 }), QSize(2560, 1440));
    QCOMPARE(announcedSize({ SOURCE, 30 }), QSize(0, 0));
}

void TestSendQuality::maxBitrateFollowsHeightAndFramerate_data()
{
    QTest::addColumn<int>("height");
    QTest::addColumn<int>("framerate");
    QTest::addColumn<int>("maxBitrate");

    QTest::newRow("480p15") << 480 << 15 << 3'500'000;
    QTest::newRow("720p30") << 720 << 30 << 3'500'000;
    QTest::newRow("721 lines") << 721 << 30 << 9'000'000;
    QTest::newRow("720p at 31") << 720 << 31 << 9'000'000;
    QTest::newRow("720p60") << 720 << 60 << 9'000'000;
    QTest::newRow("480p60") << 480 << 60 << 9'000'000;
    QTest::newRow("1080p15") << 1080 << 15 << 9'000'000;
    QTest::newRow("1440p60") << 1440 << 60 << 9'000'000;
    QTest::newRow("source at 9") << SOURCE << 9 << 3'500'000;
    QTest::newRow("source at 10") << SOURCE << 10 << 9'000'000;
    QTest::newRow("source at 15") << SOURCE << 15 << 9'000'000;
    QTest::newRow("source at 60") << SOURCE << 60 << 9'000'000;
}

void TestSendQuality::maxBitrateFollowsHeightAndFramerate()
{
    QFETCH(int, height);
    QFETCH(int, framerate);
    QFETCH(int, maxBitrate);

    const BitrateLimits limits = bitrateLimits({ height, framerate });
    QCOMPARE(limits.max, maxBitrate);
    QCOMPARE(limits.min, 500'000);
    QCOMPARE(limits.target, 600'000);
}

void TestSendQuality::bitratesAreNotScaledWithoutASmallerViewer()
{
    const SendQuality quality{ 720, 30 };
    QCOMPARE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, 0), FULL_720P);
    QCOMPARE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, PIXELS_720P), FULL_720P);
    QCOMPARE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, PIXELS_720P * 4), FULL_720P);
    QVERIFY(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, PIXELS_720P - 1) != FULL_720P);
}

void TestSendQuality::sourceQualityIsNeverScaled()
{
    const BitrateLimits full = bitrateLimits({ SOURCE, 30 });
    QCOMPARE(limitsForViewerSize(full, { SOURCE, 30 }, 2560 * 1440, 30, 320 * 180), full);
}

void TestSendQuality::bitratesScaleWithTheViewersShareOfThePicture()
{
    // Three quarters of a 1080p60 picture's pixels.
    const qint64 pixels = 1920 * 1080;
    const BitrateLimits scaled = limitsForViewerSize(FULL_1080P, { 1080, 60 }, pixels, 60, pixels * 3 / 4);
    QCOMPARE(scaled.min, 375'000);
    QCOMPARE(scaled.target, 450'000);
    QCOMPARE(scaled.max, 6'750'000);
}

void TestSendQuality::scaledBitratesAreRoundedUp()
{
    // A third of the picture: 166,666.67, 200,000 and 1,166,666.67 before rounding.
    const BitrateLimits scaled = limitsForViewerSize(FULL_720P, { 720, 30 }, 900'000, 30, 300'000);
    QCOMPARE(scaled.min, 166'667);
    QCOMPARE(scaled.target, 200'000);
    QCOMPARE(scaled.max, 1'166'667);
}

void TestSendQuality::scaledMinimumAndTargetStopAtTheFloor()
{
    // A quarter of the picture would put the minimum at 125,000 and leaves the target at
    // exactly 150,000.
    const BitrateLimits quarter = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 4);
    QCOMPARE(quarter.min, 150'000);
    QCOMPARE(quarter.target, 150'000);

    const BitrateLimits tenth = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 10);
    QCOMPARE(tenth.min, 150'000);
    QCOMPARE(tenth.target, 150'000);

    const BitrateLimits half = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 2);
    QCOMPARE(half.min, 250'000);
    QCOMPARE(half.target, 300'000);
}

void TestSendQuality::scaledMaximumKeepsRoomForThePicture()
{
    // 0.02 x 921,600 pixels x 30 frames = 552,960, above a tenth of 3,500,000.
    const BitrateLimits tenth = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 10);
    QCOMPARE(tenth.max, 552'960);

    // And under a half of it.
    const BitrateLimits half = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 2);
    QCOMPARE(half.max, 1'750'000);
}

void TestSendQuality::scaledMaximumNeverPassesTheFullMaximum()
{
    // 0.02 x 3,686,400 pixels x 60 frames is 4,423,680, more than this picture may use at all.
    const qint64 pixels = 2560 * 1440;
    const BitrateLimits scaled = limitsForViewerSize(FULL_720P, { 1440, 60 }, pixels, 60, pixels / 100);
    QCOMPARE(scaled.max, 3'500'000);

    const BitrateLimits roomy = limitsForViewerSize(FULL_1080P, { 1440, 60 }, pixels, 60, pixels / 100);
    QCOMPARE(roomy.max, 4'423'680);
}

void TestSendQuality::firstViewerSizeIsTakenUp()
{
    ViewerSize size;
    QCOMPARE(size.pixelCount(), qint64(0));
    QVERIFY(!size.offer(0));
    QVERIFY(size.offer(1));
    QCOMPARE(size.pixelCount(), qint64(1));
}

void TestSendQuality::viewerSizeWithinFivePercentIsIgnored()
{
    ViewerSize size;
    QVERIFY(size.offer(100'000));

    QVERIFY(!size.offer(100'000));
    QVERIFY(!size.offer(105'000));
    QVERIFY(!size.offer(95'000));
    QCOMPARE(size.pixelCount(), qint64(100'000));

    QVERIFY(size.offer(105'001));
    QCOMPARE(size.pixelCount(), qint64(105'001));

    ViewerSize shrinking;
    QVERIFY(shrinking.offer(100'000));
    QVERIFY(shrinking.offer(94'999));
    QCOMPARE(shrinking.pixelCount(), qint64(94'999));
}

void TestSendQuality::viewerSizeIsComparedWithTheOneInUse()
{
    // Small steps that are each ignored do not add up to a change that is not.
    ViewerSize size;
    QVERIFY(size.offer(100'000));
    QVERIFY(!size.offer(104'000));
    QVERIFY(!size.offer(105'000));
    QVERIFY(size.offer(108'000));
    QVERIFY(!size.offer(104'000));
}

void TestSendQuality::viewersGoingAwayClearsTheSize()
{
    ViewerSize size;
    QVERIFY(size.offer(100'000));
    QVERIFY(size.offer(0));
    QCOMPARE(size.pixelCount(), qint64(0));
}

void TestSendQuality::controllerStartsAtTheMaximum()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QCOMPARE(rate.bitrate(), 9'000'000);
}

void TestSendQuality::controllerIgnoresReportsBeforeTheServerHasSaid()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    for (qint64 now = 0; now <= 30'000; now += 1000)
        QVERIFY(!rate.report(-1, now));
    QCOMPARE(rate.bitrate(), 9'000'000);
}

void TestSendQuality::heavyLossLowersTheBitrate()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);

    QVERIFY(!rate.report(0.10, 0));
    QCOMPARE(rate.bitrate(), 9'000'000);

    QVERIFY(rate.report(0.101, 1000));
    QCOMPARE(rate.bitrate(), 6'300'000);
}

void TestSendQuality::heavyLossLowersNoMoreOftenThanEveryTwoSeconds()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);

    QVERIFY(rate.report(0.5, 5000));
    QVERIFY(!rate.report(0.5, 6000));
    QVERIFY(!rate.report(0.5, 6999));
    QCOMPARE(rate.bitrate(), 6'300'000);

    QVERIFY(rate.report(0.5, 7000));
    QCOMPARE(rate.bitrate(), 4'410'000);
    QVERIFY(!rate.report(0.5, 8000));
    QVERIFY(rate.report(0.5, 9000));
    QCOMPARE(rate.bitrate(), 3'087'000);
}

void TestSendQuality::bitrateNeverGoesUnderTheMinimum()
{
    SendRateController rate;
    rate.restart(500'000, 1'000'000);

    QVERIFY(rate.report(1.0, 0));
    QCOMPARE(rate.bitrate(), 700'000);
    QVERIFY(rate.report(1.0, 2000));
    QCOMPARE(rate.bitrate(), 500'000);
    QVERIFY(!rate.report(1.0, 4000));
    QVERIFY(!rate.report(1.0, 6000));
    QCOMPARE(rate.bitrate(), 500'000);
}

void TestSendQuality::moderateLossHolds()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.report(0.5, 0));

    for (qint64 now = 1000; now <= 60'000; now += 1000) {
        const double loss = now % 2000 == 0 ? 0.02 : 0.10;
        QVERIFY(!rate.report(loss, now));
    }
    QCOMPARE(rate.bitrate(), 6'300'000);
}

void TestSendQuality::tenSecondsWithoutLossRaisesTheBitrate()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.report(0.5, 0));
    QVERIFY(rate.report(0.5, 2000));
    QCOMPARE(rate.bitrate(), 4'410'000);

    for (qint64 now = 3000; now < 13'000; now += 1000)
        QVERIFY(!rate.report(0.019, now));
    QVERIFY(rate.report(0.0, 13'000));
    QCOMPARE(rate.bitrate(), 5'071'500);
}

void TestSendQuality::eachRaiseNeedsAnotherTenCleanSeconds()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.report(0.5, 0));
    QVERIFY(rate.report(0.5, 2000));

    QVERIFY(!rate.report(0, 3000));
    QVERIFY(rate.report(0, 13'000));
    QVERIFY(!rate.report(0, 14'000));
    QVERIFY(!rate.report(0, 22'999));
    QCOMPARE(rate.bitrate(), 5'071'500);

    QVERIFY(rate.report(0, 23'000));
    QCOMPARE(rate.bitrate(), 5'832'225);
}

void TestSendQuality::raisesStopAtTheMaximum()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.report(0.5, 0));
    QCOMPARE(rate.bitrate(), 6'300'000);

    QVERIFY(!rate.report(0, 1000));
    QVERIFY(rate.report(0, 11'000));
    QCOMPARE(rate.bitrate(), 7'245'000);
    QVERIFY(rate.report(0, 21'000));
    QCOMPARE(rate.bitrate(), 8'331'750);
    QVERIFY(rate.report(0, 31'000));
    QCOMPARE(rate.bitrate(), 9'000'000);

    QVERIFY(!rate.report(0, 41'000));
    QVERIFY(!rate.report(0, 51'000));
    QCOMPARE(rate.bitrate(), 9'000'000);
}

void TestSendQuality::lossInterruptsTheCleanStretch()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.report(0.5, 0));

    QVERIFY(!rate.report(0, 1000));
    QVERIFY(!rate.report(0.05, 9000));
    QVERIFY(!rate.report(0, 10'000));
    QVERIFY(!rate.report(0, 19'999));
    QCOMPARE(rate.bitrate(), 6'300'000);

    QVERIFY(rate.report(0, 20'000));
    QCOMPARE(rate.bitrate(), 7'245'000);
}

void TestSendQuality::newLimitsKeepTheShareOfTheMaximum()
{
    SendRateController untouched;
    untouched.restart(500'000, 9'000'000);
    QVERIFY(untouched.setLimits(250'000, 4'500'000));
    QCOMPARE(untouched.bitrate(), 4'500'000);
    QVERIFY(untouched.setLimits(500'000, 9'000'000));
    QCOMPARE(untouched.bitrate(), 9'000'000);
    QVERIFY(!untouched.setLimits(500'000, 9'000'000));

    SendRateController lowered;
    lowered.restart(500'000, 9'000'000);
    QVERIFY(lowered.report(0.5, 0));
    QVERIFY(lowered.setLimits(250'000, 4'500'000));
    QCOMPARE(lowered.bitrate(), 3'150'000);

    // The new minimum still holds.
    QVERIFY(lowered.setLimits(400'000, 500'000));
    QCOMPARE(lowered.bitrate(), 400'000);
}

void TestSendQuality::aMaximumUnderTheMinimumIsStillAMaximum()
{
    // What the viewer-size rule gives a 480p15 picture shown very small.
    const BitrateLimits scaled = limitsForViewerSize(FULL_720P, { 480, 15 }, 640 * 480, 15, 640 * 480 / 100);
    QCOMPARE(scaled.min, 150'000);
    QCOMPARE(scaled.max, 92'160);

    SendRateController rate;
    rate.restart(scaled.min, scaled.max);
    QCOMPARE(rate.bitrate(), 92'160);
    QVERIFY(!rate.report(0.5, 0));
    QCOMPARE(rate.bitrate(), 92'160);
}

void TestSendQuality::restartGoesBackToTheMaximum()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.report(0.5, 0));

    rate.restart(500'000, 3'500'000);
    QCOMPARE(rate.bitrate(), 3'500'000);
    // The two seconds between decreases start over as well.
    QVERIFY(rate.report(0.5, 100));
    QCOMPARE(rate.bitrate(), 2'450'000);
}

QTEST_GUILESS_MAIN(TestSendQuality)
#include "tst_SendQuality.moc"
