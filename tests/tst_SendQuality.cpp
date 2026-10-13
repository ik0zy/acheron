// Tests for what a screen share is sent at: which qualities an account may pick, the size and
// bitrates announced for each, how the bitrates follow the size viewers show the stream at, and
// the controller that moves the encoder's bitrate by reported loss. The quality rules are the
// official client's (docs/video-re/re-voice-gateway-video.md).
//
// And for what the account's own camera is encoded at: the ladder of sizes by how much of the
// picture its viewers want, the lower frame rate of an owner who is not heard, and the plan
// that puts those together with the loss the server reports.

#include "Core/Video/SendQuality.hpp"
#include "Core/Video/SendRateController.hpp"

#include <QTest>

using namespace Acheron::Core::Video;

// allowed[premium_type][premium_tier]
using TierTable = std::array<std::array<bool, 4>, 4>;
Q_DECLARE_METATYPE(TierTable)
Q_DECLARE_METATYPE(CameraLayer)

namespace {

constexpr int SOURCE = SendQuality::SOURCE_HEIGHT;
constexpr qint64 PIXELS_720P = 1280 * 720;

const BitrateLimits FULL_720P{ 500'000, 600'000, 3'500'000 };
const BitrateLimits FULL_1080P{ 500'000, 600'000, 9'000'000 };

constexpr TierTable EVERYONE{ { { true, true, true, true }, { true, true, true, true }, { true, true, true, true }, { true, true, true, true } } };
constexpr TierTable BOOST_ONE_OR_NITRO{ { { false, true, true, true }, { true, true, true, true }, { true, true, true, true }, { false, true, true, true } } };
constexpr TierTable BOOST_TWO_OR_NITRO{ { { false, false, true, true }, { true, true, true, true }, { true, true, true, true }, { false, false, true, true } } };
constexpr TierTable FULL_NITRO_ONLY{ { { false, false, false, false }, { false, false, false, false }, { true, true, true, true }, { false, false, false, false } } };

const std::array<CameraLayer, 9> CAMERA_LADDER{ {
        { 100, QSize(1280, 720), 2'500'000 },
        { 90, QSize(896, 504), 1'225'000 },
        { 80, QSize(672, 378), 689'062 },
        { 70, QSize(640, 360), 625'000 },
        { 60, QSize(512, 288), 400'000 },
        { 50, QSize(416, 234), 264'062 },
        { 40, QSize(384, 216), 225'000 },
        { 30, QSize(320, 180), 156'250 },
        { 20, QSize(256, 144), 150'000 },
} };

const QSize FULL_CAMERA(1280, 720);

// A camera whose owner goes on speaking, so that only what a test does lowers its frame rate.
CameraSendPlan planOfASpeaker()
{
    CameraSendPlan plan;
    plan.start(0);
    plan.setSpeaking(true, 0);
    return plan;
}

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
    void theReceiversEstimateIsACeiling();
    void theReceiversEstimateIsFollowedDownAtOnceAndUpByWholeSteps();
    void liftingTheReceiversEstimateLeavesNoMark();
    void lossLowersFromWhatTheReceiversEstimateAllows();
    void newLimitsKeepTheShareOfTheMaximum();
    void aMaximumUnderTheMinimumIsStillAMaximum();
    void restartGoesBackToTheMaximum();

    void cameraLadderHasARowForEachWant_data();
    void cameraLadderHasARowForEachWant();
    void aWantBetweenRowsTakesTheRowBelow();
    void aWantUnderTwentyTakesTheLowestRow();
    void cameraMaxBitrateIsTheFullPicturesByShareOfTheArea();
    void cameraFramerateIsLoweredForAMutedOwner_data();
    void cameraFramerateIsLoweredForAMutedOwner();

    void theCameraStartsWithTheWholePictureAtItsMaximum();
    void selfMuteLowersTheFramerateAtOnce();
    void fifteenSecondsOfSilenceLowerTheFramerate();
    void silenceIsCountedFromTheLastWord();
    void speakingRestoresTheFramerateAtOnce();
    void aMutedCameraWantedAtTwentyOrLessIsSentAtTwelve();
    void theFramerateIsNotLoweredWhileAScreenIsShared();
    void aNewWantIsTakenUpTwoSecondsAfterTheLastChange();
    void wantsInQuickSuccessionAreGatheredIntoOne();
    void aWantWithinTheRowInUseChangesNothing();
    void lossMovesTheCameraBitrateDownToItsMinimum();
    void aBitrateChangeWaitsOutTheTwoSecondsToo();
    void aNewRowKeepsTheBitratesShareOfItsMaximum();
    void thePlanSaysWhenItNextChangesByItself();
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

void TestSendQuality::cameraLadderHasARowForEachWant_data()
{
    QTest::addColumn<CameraLayer>("row");

    for (const CameraLayer &row : CAMERA_LADDER)
        QTest::addRow("want %d", row.want) << row;
}

void TestSendQuality::cameraLadderHasARowForEachWant()
{
    QFETCH(CameraLayer, row);

    const CameraLayer chosen = cameraLayerFor(row.want);
    QCOMPARE(chosen.want, row.want);
    QCOMPARE(chosen.size, row.size);
    QCOMPARE(chosen.maxBitrate, row.maxBitrate);
    QCOMPARE(chosen.size.width() * 9, chosen.size.height() * 16);
}

void TestSendQuality::aWantBetweenRowsTakesTheRowBelow()
{
    for (size_t i = 0; i + 1 < CAMERA_LADDER.size(); i++) {
        const CameraLayer &row = CAMERA_LADDER[i];
        const CameraLayer &below = CAMERA_LADDER[i + 1];
        QCOMPARE(cameraLayerFor(row.want - 1), below);
        QCOMPARE(cameraLayerFor(below.want + 1), below);
    }

    // More than everything is still everything.
    QCOMPARE(cameraLayerFor(101), CAMERA_LADDER.front());
}

void TestSendQuality::aWantUnderTwentyTakesTheLowestRow()
{
    for (const int want : { 19, 10, 1, 0, -1 })
        QCOMPARE(cameraLayerFor(want), CAMERA_LADDER.back());
}

void TestSendQuality::cameraMaxBitrateIsTheFullPicturesByShareOfTheArea()
{
    QCOMPARE(cameraMaxBitrate(1280 * 720), 2'500'000);
    QCOMPARE(cameraMaxBitrate(640 * 360), 625'000);
    // Whatever is left of a bit is dropped.
    QCOMPARE(cameraMaxBitrate(672 * 378), 689'062);
    QCOMPARE(cameraMaxBitrate(416 * 234), 264'062);
    // The lowest row's share would be 100,000.
    QCOMPARE(cameraMaxBitrate(256 * 144), 150'000);
    QCOMPARE(cameraMaxBitrate(0), 150'000);

    QCOMPARE(CAMERA_MIN_BITRATE, 150'000);
    QCOMPARE(CAMERA_CAPTURE_SIZE, FULL_CAMERA);
    QCOMPARE(CAMERA_FRAMERATE, 30);
}

void TestSendQuality::cameraFramerateIsLoweredForAMutedOwner_data()
{
    QTest::addColumn<int>("want");
    QTest::addColumn<bool>("muted");
    QTest::addColumn<int>("framerate");

    QTest::newRow("heard, all wanted") << 100 << false << 30;
    QTest::newRow("heard, little wanted") << 20 << false << 30;
    QTest::newRow("heard, nothing wanted") << 0 << false << 30;
    QTest::newRow("muted, all wanted") << 100 << true << 20;
    QTest::newRow("muted, 30 wanted") << 30 << true << 20;
    QTest::newRow("muted, 21 wanted") << 21 << true << 20;
    QTest::newRow("muted, 20 wanted") << 20 << true << 12;
    QTest::newRow("muted, 19 wanted") << 19 << true << 12;
    QTest::newRow("muted, nothing wanted") << 0 << true << 12;
}

void TestSendQuality::cameraFramerateIsLoweredForAMutedOwner()
{
    QFETCH(int, want);
    QFETCH(bool, muted);
    QFETCH(int, framerate);

    QCOMPARE(cameraFramerate(want, muted), framerate);
}

void TestSendQuality::theCameraStartsWithTheWholePictureAtItsMaximum()
{
    CameraSendPlan plan;
    plan.start(1000);

    const CameraEncoding first = plan.advance(1000);
    QCOMPARE(first.size, FULL_CAMERA);
    QCOMPARE(first.framerate, 30);
    QCOMPARE(first.bitrate, 2'500'000);
    QCOMPARE(plan.servedWant(), 100);
    QCOMPARE(plan.maxBitrate(), 2'500'000);

    // A camera turned on again starts over, whatever the last one was down to.
    plan.setWant(20);
    plan.reportLoss(0.5, 4000);
    QCOMPARE(plan.advance(4000).size, QSize(256, 144));
    plan.start(9000);
    QCOMPARE(plan.advance(9000), first);
}

void TestSendQuality::selfMuteLowersTheFramerateAtOnce()
{
    CameraSendPlan plan = planOfASpeaker();
    QCOMPARE(plan.advance(100).framerate, 30);

    plan.setSelfMuted(true);
    QVERIFY(plan.countsAsMuted(100));
    const CameraEncoding muted = plan.advance(100);
    QCOMPARE(muted.framerate, 20);
    // Nothing else of the picture changes with it.
    QCOMPARE(muted.size, FULL_CAMERA);
    QCOMPARE(muted.bitrate, 2'500'000);

    plan.setSelfMuted(false);
    QVERIFY(!plan.countsAsMuted(101));
    QCOMPARE(plan.advance(101).framerate, 30);
}

void TestSendQuality::fifteenSecondsOfSilenceLowerTheFramerate()
{
    // Nothing has been said since the camera was turned on.
    CameraSendPlan plan;
    plan.start(1000);

    QCOMPARE(plan.advance(15'999).framerate, 30);
    QVERIFY(!plan.countsAsMuted(15'999));
    QCOMPARE(plan.advance(16'000).framerate, 20);
    QVERIFY(plan.countsAsMuted(16'000));
    QCOMPARE(plan.advance(600'000).framerate, 20);
}

void TestSendQuality::silenceIsCountedFromTheLastWord()
{
    CameraSendPlan plan;
    plan.start(0);
    plan.setSpeaking(true, 5000);
    // Speaking, for however long, keeps the full rate.
    QCOMPARE(plan.advance(60'000).framerate, 30);

    plan.setSpeaking(false, 60'000);
    QCOMPARE(plan.advance(74'999).framerate, 30);
    QCOMPARE(plan.advance(75'000).framerate, 20);

    // Being told again that nothing is said does not start the count over.
    plan.setSpeaking(true, 80'000);
    plan.setSpeaking(false, 81'000);
    plan.setSpeaking(false, 90'000);
    QCOMPARE(plan.advance(95'999).framerate, 30);
    QCOMPARE(plan.advance(96'000).framerate, 20);
}

void TestSendQuality::speakingRestoresTheFramerateAtOnce()
{
    CameraSendPlan plan;
    plan.start(0);
    QCOMPARE(plan.advance(20'000).framerate, 20);

    plan.setSpeaking(true, 20'001);
    QCOMPARE(plan.advance(20'001).framerate, 30);

    // Not while self-muted, though: what is said then is not heard.
    plan.setSelfMuted(true);
    QCOMPARE(plan.advance(20'002).framerate, 20);
}

void TestSendQuality::aMutedCameraWantedAtTwentyOrLessIsSentAtTwelve()
{
    CameraSendPlan plan = planOfASpeaker();
    plan.setSelfMuted(true);

    plan.setWant(21);
    const CameraEncoding lowestRow = plan.advance(2000);
    QCOMPARE(lowestRow.size, QSize(256, 144));
    QCOMPARE(lowestRow.framerate, 20);

    // The same row, and still a change for the encoder.
    plan.setWant(20);
    QCOMPARE(plan.advance(3999).framerate, 20);
    const CameraEncoding slowest = plan.advance(4000);
    QCOMPARE(slowest.size, QSize(256, 144));
    QCOMPARE(slowest.framerate, 12);

    plan.setSelfMuted(false);
    QCOMPARE(plan.advance(4001).framerate, 30);
}

void TestSendQuality::theFramerateIsNotLoweredWhileAScreenIsShared()
{
    CameraSendPlan plan;
    plan.start(0);
    plan.setSelfMuted(true);
    plan.setSharingScreen(true);

    QVERIFY(!plan.countsAsMuted(60'000));
    QCOMPARE(plan.advance(60'000).framerate, 30);

    plan.setSharingScreen(false);
    QCOMPARE(plan.advance(60'000).framerate, 20);

    // Silence does not lower it either.
    plan.setSelfMuted(false);
    QCOMPARE(plan.advance(60'001).framerate, 20);
    plan.setSharingScreen(true);
    QCOMPARE(plan.advance(60'002).framerate, 30);
}

void TestSendQuality::aNewWantIsTakenUpTwoSecondsAfterTheLastChange()
{
    CameraSendPlan plan = planOfASpeaker();

    plan.setWant(50);
    QCOMPARE(plan.advance(1999).size, FULL_CAMERA);
    QCOMPARE(plan.servedWant(), 100);

    const CameraEncoding lowered = plan.advance(2000);
    QCOMPARE(lowered.size, QSize(416, 234));
    QCOMPARE(lowered.framerate, 30);
    QCOMPARE(lowered.bitrate, 264'062);
    QCOMPARE(plan.servedWant(), 50);
    QCOMPARE(plan.maxBitrate(), 264'062);

    // The two seconds start again with every change that is made.
    plan.setWant(100);
    QCOMPARE(plan.advance(3999), lowered);
    const CameraEncoding raised = plan.advance(4000);
    QCOMPARE(raised.size, FULL_CAMERA);
    QCOMPARE(raised.bitrate, 2'500'000);
}

void TestSendQuality::wantsInQuickSuccessionAreGatheredIntoOne()
{
    CameraSendPlan plan = planOfASpeaker();

    // Long after the last change, the first of them is taken up at once.
    plan.setWant(50);
    QCOMPARE(plan.advance(5000).size, QSize(416, 234));

    plan.setWant(30);
    QCOMPARE(plan.advance(5100).size, QSize(416, 234));
    plan.setWant(70);
    QCOMPARE(plan.advance(5500).size, QSize(416, 234));
    plan.setWant(90);
    QCOMPARE(plan.advance(6999).size, QSize(416, 234));

    const CameraEncoding settled = plan.advance(7000);
    QCOMPARE(settled.size, QSize(896, 504));
    QCOMPARE(settled.bitrate, 1'225'000);

    // A want that comes back to the one in use leaves nothing to do.
    plan.setWant(40);
    plan.setWant(90);
    QVERIFY(!plan.nextChangeIn(7100));
    QCOMPARE(plan.advance(20'000), settled);
}

void TestSendQuality::aWantWithinTheRowInUseChangesNothing()
{
    CameraSendPlan plan = planOfASpeaker();
    plan.setWant(90);
    const CameraEncoding atNinety = plan.advance(2000);

    plan.setWant(95);
    QCOMPARE(plan.servedWant(), 95);
    QVERIFY(!plan.nextChangeIn(2100));
    QCOMPARE(plan.advance(2100), atNinety);

    // And does not put off a change that does matter to the encoder.
    plan.setWant(80);
    QCOMPARE(plan.nextChangeIn(2100), std::optional<qint64>(1900));
    QCOMPARE(plan.advance(4000).size, QSize(672, 378));
}

void TestSendQuality::lossMovesTheCameraBitrateDownToItsMinimum()
{
    CameraSendPlan plan = planOfASpeaker();

    plan.reportLoss(0.5, 3000);
    QCOMPARE(plan.advance(3000).bitrate, 1'750'000);
    plan.reportLoss(0.5, 5000);
    const CameraEncoding lowered = plan.advance(5000);
    QCOMPARE(lowered.bitrate, 1'225'000);
    // The picture keeps its size: only the want changes that.
    QCOMPARE(lowered.size, FULL_CAMERA);

    // Before the server has said anything of loss there is nothing to act on.
    plan.reportLoss(-1, 7000);
    QCOMPARE(plan.advance(7000).bitrate, 1'225'000);

    qint64 now = 7000;
    for (int i = 0; i < 12; i++) {
        now += 2000;
        plan.reportLoss(0.5, now);
        plan.advance(now);
    }
    QCOMPARE(plan.advance(now).bitrate, 150'000);
}

void TestSendQuality::aBitrateChangeWaitsOutTheTwoSecondsToo()
{
    CameraSendPlan plan = planOfASpeaker();

    plan.reportLoss(0.5, 500);
    QCOMPARE(plan.advance(500).bitrate, 2'500'000);
    QCOMPARE(plan.advance(1999).bitrate, 2'500'000);
    QCOMPARE(plan.advance(2000).bitrate, 1'750'000);
}

void TestSendQuality::aNewRowKeepsTheBitratesShareOfItsMaximum()
{
    CameraSendPlan plan = planOfASpeaker();
    plan.reportLoss(0.5, 3000);
    QCOMPARE(plan.advance(3000).bitrate, 1'750'000);

    plan.setWant(70);
    const CameraEncoding smaller = plan.advance(5000);
    QCOMPARE(smaller.size, QSize(640, 360));
    QCOMPARE(smaller.bitrate, 437'500);

    // On the lowest row the minimum and the maximum are one and the same.
    plan.setWant(20);
    QCOMPARE(plan.advance(7000).bitrate, 150'000);
    plan.reportLoss(0.5, 9000);
    QCOMPARE(plan.advance(9000).bitrate, 150'000);
}

void TestSendQuality::thePlanSaysWhenItNextChangesByItself()
{
    CameraSendPlan plan;
    plan.start(1000);

    // Silent since the camera came on: the frame rate drops fifteen seconds after that.
    QCOMPARE(plan.nextChangeIn(1000), std::optional<qint64>(15'000));
    QCOMPARE(plan.nextChangeIn(10'000), std::optional<qint64>(6000));

    // A want that waits for the two seconds comes first.
    plan.setWant(50);
    QCOMPARE(plan.nextChangeIn(1500), std::optional<qint64>(1500));
    plan.advance(3000);
    QCOMPARE(plan.nextChangeIn(3000), std::optional<qint64>(13'000));

    // Once the rate has dropped, or while it cannot, nothing is due.
    plan.advance(16'000);
    QVERIFY(!plan.nextChangeIn(16'000));
    plan.setSpeaking(true, 17'000);
    QVERIFY(!plan.nextChangeIn(17'000));
    plan.setSpeaking(false, 18'000);
    QCOMPARE(plan.nextChangeIn(18'000), std::optional<qint64>(15'000));
    plan.setSelfMuted(true);
    QVERIFY(!plan.nextChangeIn(18'000));
    plan.setSelfMuted(false);
    plan.setSharingScreen(true);
    QVERIFY(!plan.nextChangeIn(18'000));

    // A change that is overdue is due now.
    plan.setWant(100);
    QCOMPARE(plan.nextChangeIn(30'000), std::optional<qint64>(0));
}

void TestSendQuality::theReceiversEstimateIsACeiling()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);

    // One above what is in use changes nothing.
    QVERIFY(!rate.setReceiverLimit(20'000'000));
    QCOMPARE(rate.bitrate(), 9'000'000);

    QVERIFY(rate.setReceiverLimit(3'000'000));
    QCOMPARE(rate.bitrate(), 3'000'000);

    // Never under the minimum, whatever it says.
    QVERIFY(rate.setReceiverLimit(100'000));
    QCOMPARE(rate.bitrate(), 500'000);

    // New limits for the video are bounded by it as well.
    rate.setReceiverLimit(0);
    QVERIFY(rate.setReceiverLimit(3'000'000));
    QVERIFY(!rate.setLimits(500'000, 6'000'000));
    QCOMPARE(rate.bitrate(), 3'000'000);
    QVERIFY(rate.setLimits(500'000, 2'000'000));
    QCOMPARE(rate.bitrate(), 2'000'000);

    // Starting over forgets it.
    rate.restart(500'000, 9'000'000);
    QCOMPARE(rate.bitrate(), 9'000'000);
}

void TestSendQuality::theReceiversEstimateIsFollowedDownAtOnceAndUpByWholeSteps()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.setReceiverLimit(4'000'000));

    QVERIFY(rate.setReceiverLimit(3'990'000));
    QCOMPARE(rate.bitrate(), 3'990'000);

    // An estimate that wanders about the same place does not have the encoder restarted for it.
    for (int estimate : { 4'000'000, 4'300'000, 3'995'000, 4'580'000 })
        QVERIFY(!rate.setReceiverLimit(estimate));
    QCOMPARE(rate.bitrate(), 3'990'000);

    QVERIFY(rate.setReceiverLimit(4'600'000));
    QCOMPARE(rate.bitrate(), 4'600'000);
}

void TestSendQuality::liftingTheReceiversEstimateLeavesNoMark()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.setReceiverLimit(1'000'000));

    QVERIFY(rate.setReceiverLimit(0));
    QCOMPARE(rate.bitrate(), 9'000'000);
    QVERIFY(!rate.setReceiverLimit(0));
}

void TestSendQuality::lossLowersFromWhatTheReceiversEstimateAllows()
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    QVERIFY(rate.setReceiverLimit(2'000'000));

    // Not from the nine megabits that losses alone had allowed, which would change nothing.
    QVERIFY(rate.report(0.5, 0));
    QCOMPARE(rate.bitrate(), 1'400'000);

    // A clean stretch then raises it as ever, and the estimate still bounds that.
    for (qint64 now = 1000; now < 11'000; now += 1000)
        QVERIFY(!rate.report(0.0, now));
    QVERIFY(rate.report(0.0, 11'000));
    QCOMPARE(rate.bitrate(), 1'610'000);
    QVERIFY(rate.report(0.0, 21'000));
    QVERIFY(rate.report(0.0, 31'000));
    QCOMPARE(rate.bitrate(), 2'000'000);
    QVERIFY(!rate.report(0.0, 41'000));
}

QTEST_GUILESS_MAIN(TestSendQuality)
#include "tst_SendQuality.moc"
