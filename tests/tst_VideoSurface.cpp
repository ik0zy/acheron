// Tests for the software video surface: a decoded frame handed to its sink ends up on screen in
// the right place, at the right size and in the right colours, and the surface reports how
// large the picture is drawn.

#include "UI/Video/VideoSurface.hpp"

#include <QImage>
#include <QSignalSpy>
#include <QTest>

extern "C" {
#include <libavutil/frame.h>
}

using Acheron::UI::VideoSurface;

namespace {

// A frame of one colour, given as limited-range Y, Cb and Cr.
AVFrame *solidFrame(int width, int height, uint8_t y, uint8_t cb, uint8_t cr)
{
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    for (int row = 0; row < height; row++)
        memset(frame->data[0] + row * frame->linesize[0], y, static_cast<size_t>(width));
    for (int row = 0; row < height / 2; row++) {
        memset(frame->data[1] + row * frame->linesize[1], cb, static_cast<size_t>(width / 2));
        memset(frame->data[2] + row * frame->linesize[2], cr, static_cast<size_t>(width / 2));
    }
    return frame;
}

void present(VideoSurface &surface, AVFrame *frame)
{
    surface.sink()->present(frame);
    av_frame_free(&frame);
    QCoreApplication::processEvents();
}

QImage shot(VideoSurface &surface)
{
    return surface.grab().toImage().convertToFormat(QImage::Format_RGB32);
}

bool isNear(QRgb pixel, int red, int green, int blue)
{
    constexpr int TOLERANCE = 12;
    return qAbs(qRed(pixel) - red) <= TOLERANCE && qAbs(qGreen(pixel) - green) <= TOLERANCE && qAbs(qBlue(pixel) - blue) <= TOLERANCE;
}

} // namespace

class TestVideoSurface : public QObject
{
    Q_OBJECT
private slots:
    void emptySurfaceIsBlack();
    void frameFillsAMatchingSurface();
    void narrowerFrameIsCentredBetweenBars();
    void coloursFollowTheVideoStandard();
    void geometryIsReported();
    void resizeRedrawsTheLastFrame();
    void clearForgetsThePicture();
    void messagesDoNotDisturbThePicture();
    void avatarStandsInUntilThereIsAPicture();
    void speakingRingAndLiveMarkerAreDrawn();
    void statisticsAreLeftOffASmallTile();
};

void TestVideoSurface::emptySurfaceIsBlack()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 0, 0));
}

void TestVideoSurface::frameFillsAMatchingSurface()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    // White, from a frame twice the surface's size.
    present(surface, solidFrame(640, 360, 235, 128, 128));

    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(0, 0), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() - 1, image.height() - 1), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));
}

void TestVideoSurface::narrowerFrameIsCentredBetweenBars()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    // 4:3 in a 16:9 surface: 240 wide, with a 40 pixel bar on each side.
    present(surface, solidFrame(640, 480, 235, 128, 128));

    const QImage image = shot(surface);
    const qreal ratio = image.devicePixelRatio();
    const int middle = image.height() / 2;
    QVERIFY(isNear(image.pixel(qRound(20 * ratio), middle), 0, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(300 * ratio), middle), 0, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(45 * ratio), middle), 255, 255, 255));
    QVERIFY(isNear(image.pixel(qRound(275 * ratio), middle), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() / 2, 0), 255, 255, 255));
}

void TestVideoSurface::coloursFollowTheVideoStandard()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    // Pure red is a different triple in BT.709 (HD) and BT.601 (SD); untagged frames are read
    // by their height.
    present(surface, solidFrame(1280, 720, 63, 102, 240));
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

    present(surface, solidFrame(640, 360, 81, 90, 240));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
}

void TestVideoSurface::geometryIsReported()
{
    VideoSurface surface;
    surface.resize(320, 180);
    QSignalSpy geometry(&surface, &VideoSurface::geometryChanged);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    const qreal ratio = surface.devicePixelRatioF();
    const int fullArea = qRound(320 * ratio) * qRound(180 * ratio);

    // Before a frame, the whole surface stands in for the picture.
    QVERIFY(!geometry.isEmpty());
    QCOMPARE(geometry.last().at(0).toInt(), fullArea);
    QCOMPARE(geometry.last().at(1).toBool(), true);

    // A 4:3 picture covers three quarters of a 16:9 surface.
    present(surface, solidFrame(640, 480, 235, 128, 128));
    QCOMPARE(geometry.last().at(0).toInt(), qRound(240 * ratio) * qRound(180 * ratio));

    surface.hide();
    QCOMPARE(geometry.last().at(0).toInt(), 0);
    QCOMPARE(geometry.last().at(1).toBool(), false);
}

void TestVideoSurface::resizeRedrawsTheLastFrame()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    present(surface, solidFrame(640, 360, 235, 128, 128));

    // No new frame arrives after the resize; the surface has to rescale the one it has.
    surface.resize(480, 270);
    QTest::qWait(300);

    const QImage image = shot(surface);
    QCOMPARE(image.size(), QSize(480, 270) * image.devicePixelRatio());
    QVERIFY(isNear(image.pixel(1, 1), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));
}

void TestVideoSurface::clearForgetsThePicture()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    present(surface, solidFrame(640, 360, 235, 128, 128));

    surface.clear();
    QCoreApplication::processEvents();

    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 0, 0));
}

void TestVideoSurface::messagesDoNotDisturbThePicture()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    present(surface, solidFrame(640, 360, 235, 128, 128));

    surface.setStatsText(QStringLiteral("Codec: H264"));
    QImage image = shot(surface);
    // The statistics sit in the top left corner on a dark backing; the rest is untouched.
    QVERIFY(!isNear(image.pixel(qRound(10 * image.devicePixelRatio()), qRound(10 * image.devicePixelRatio())), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));

    surface.setStatsText({});
    surface.setMessage(QStringLiteral("Stream Paused"), QStringLiteral("sit tight"));
    image = shot(surface);
    // A message dims the whole picture.
    QVERIFY(!isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));

    surface.setMessage({});
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));
}

void TestVideoSurface::avatarStandsInUntilThereIsAPicture()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    QPixmap avatar(64, 64);
    avatar.fill(QColor(0, 200, 0));
    surface.setPlaceholder(avatar);

    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));
    QVERIFY(isNear(image.pixel(4, 4), 0, 0, 0));

    present(surface, solidFrame(640, 360, 235, 128, 128));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

    // And comes back when the picture is gone.
    surface.clear();
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));
}

void TestVideoSurface::speakingRingAndLiveMarkerAreDrawn()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0, 0, 0));

    surface.setSpeaking(true);
    image = shot(surface);
    QVERIFY(!isNear(image.pixel(1, image.height() / 2), 0, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 0, 0));

    surface.setSpeaking(false);
    surface.setLive(true);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0, 0, 0));
    // Somewhere in the top right corner there is now red.
    bool red = false;
    for (int x = image.width() - 60; x < image.width() - 8 && !red; x++) {
        for (int y = 8; y < 30 && !red; y++)
            red = qRed(image.pixel(x, y)) > 200 && qGreen(image.pixel(x, y)) < 110;
    }
    QVERIFY(red);
}

void TestVideoSurface::statisticsAreLeftOffASmallTile()
{
    const QString stats = QStringLiteral("Codec: H264\nResolution: 1280x720\nFrame rate: 30.0 fps\nBitrate: 900 kbps");

    VideoSurface large;
    large.resize(480, 270);
    large.show();
    QVERIFY(QTest::qWaitForWindowExposed(&large));
    present(large, solidFrame(640, 360, 235, 128, 128));
    large.setStatsText(stats);
    QImage image = shot(large);
    QVERIFY(!isNear(image.pixel(12, 12), 255, 255, 255));

    VideoSurface small;
    small.resize(160, 90);
    small.show();
    QVERIFY(QTest::qWaitForWindowExposed(&small));
    present(small, solidFrame(640, 360, 235, 128, 128));
    small.setStatsText(stats);
    image = shot(small);
    QVERIFY(isNear(image.pixel(12, 12), 255, 255, 255));
}

QTEST_MAIN(TestVideoSurface)
#include "tst_VideoSurface.moc"
