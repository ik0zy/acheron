// Tests for the software video surface: a decoded frame handed to its sink ends up on screen in
// the right place, at the right size and in the right colours, and the surface reports how
// large the picture is drawn.

#include "UI/Video/VideoSurface.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QWindow>
#include <QTest>

#ifdef Q_OS_WIN
// Before ffmpeg's header, which would otherwise pull it in as C.
#include <d3d11.h>
#undef small
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if defined(Q_OS_WIN) && __has_include(<libavutil/hwcontext_d3d11va.h>)
#include <libavutil/hwcontext_d3d11va.h>
#define HAVE_D3D11VA 1
#endif
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

#ifdef HAVE_D3D11VA
// The same picture as a hardware decoder hands it over: in one of its textures on the graphics
// card. Null on a machine with no Direct3D 11 video device.
AVFrame *solidFrameOnCard(int width, int height, uint8_t y, uint8_t cb, uint8_t cr)
{
    AVBufferRef *device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
        return nullptr;
    AVBufferRef *pool = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);
    auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = width;
    frames->height = height;
    frames->initial_pool_size = 2;
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = D3D11_BIND_DECODER;

    AVFrame *memory = av_frame_alloc();
    memory->format = AV_PIX_FMT_NV12;
    memory->width = width;
    memory->height = height;
    av_frame_get_buffer(memory, 0);
    for (int row = 0; row < height; row++)
        memset(memory->data[0] + row * memory->linesize[0], y, static_cast<size_t>(width));
    for (int row = 0; row < height / 2; row++) {
        uint8_t *pair = memory->data[1] + row * memory->linesize[1];
        for (int column = 0; column < width / 2; column++) {
            *pair++ = cb;
            *pair++ = cr;
        }
    }

    AVFrame *frame = av_frame_alloc();
    if (av_hwframe_ctx_init(pool) < 0 || av_hwframe_get_buffer(pool, frame, 0) < 0 || av_hwframe_transfer_data(frame, memory, 0) < 0)
        av_frame_free(&frame);
    av_frame_free(&memory);
    av_buffer_unref(&pool);
    return frame;
}
#endif

void present(VideoSurface &surface, AVFrame *frame)
{
    surface.sink()->present(frame);
    av_frame_free(&frame);
    QCoreApplication::processEvents();
}

QImage shot(VideoSurface &surface)
{
    // With the graphics card the picture is in a window of its own, which is up a moment
    // after the surface is.
    QImage image = surface.snapshot();
    for (int i = 0; i < 200 && image.isNull(); i++) {
        QTest::qWait(10);
        image = surface.snapshot();
    }
    return image.convertToFormat(QImage::Format_RGB32);
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
    void frameOnTheGraphicsCardIsDrawnFromThere();
    void manySurfacesDrawFromTheGraphicsCard();
    void pictureSurvivesAMoveToAnotherWindow();
    void clickAndDoubleClickAreReported();
    void geometryIsReported();
    void resizeRedrawsTheLastFrame();
    void clearForgetsThePicture();
    void messagesDoNotDisturbThePicture();
    void avatarStandsInUntilThereIsAPicture();
    void speakingRingIsDrawn();
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

void TestVideoSurface::frameOnTheGraphicsCardIsDrawnFromThere()
{
#ifdef HAVE_D3D11VA
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    // The surface knows whether it can once it has drawn.
    shot(surface);
    if (!surface.sink()->acceptsHardwareFrames(AV_PIX_FMT_D3D11))
        QSKIP("This surface cannot draw a Direct3D picture where it is");
    QVERIFY(!surface.sink()->acceptsHardwareFrames(AV_PIX_FMT_DXVA2_VLD));

    // Red in BT.709, as an HD frame is read.
    AVFrame *frame = solidFrameOnCard(1280, 720, 63, 102, 240);
    if (!frame)
        QSKIP("No Direct3D 11 video device");
    // The picture is made ready off this thread, and drawn once it is.
    present(surface, frame);
    QTRY_VERIFY(surface.drawsFromCard());
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(0, 0), 255, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() - 1, image.height() - 1), 255, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

    // Red in BT.601, 4:3, from another device: bars at the sides.
    frame = solidFrameOnCard(640, 480, 81, 90, 240);
    QVERIFY(frame);
    const qreal ratio = image.devicePixelRatio();
    const int middle = image.height() / 2;
    present(surface, frame);
    QTRY_VERIFY(isNear(shot(surface).pixel(qRound(20 * ratio), middle), 0, 0, 0));
    image = shot(surface);
    QVERIFY(surface.drawsFromCard());
    QVERIFY(isNear(image.pixel(qRound(45 * ratio), middle), 255, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(275 * ratio), middle), 255, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(300 * ratio), middle), 0, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() / 2, 0), 255, 0, 0));

    // The overlays go over it as over any picture.
    surface.setSpeaking(true);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, middle), 255, 0, 0));
    QVERIFY(!isNear(image.pixel(1, middle), 0, 0, 0));
    surface.setSpeaking(false);

    // A frame in memory after that is uploaded as before.
    present(surface, solidFrame(640, 360, 235, 128, 128));
    image = shot(surface);
    QVERIFY(!surface.drawsFromCard());
    QVERIFY(isNear(image.pixel(image.width() / 2, middle), 255, 255, 255));

    surface.clear();
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, middle), 0, 0, 0));
#else
    QSKIP("Direct3D 11 only");
#endif
}

void TestVideoSurface::manySurfacesDrawFromTheGraphicsCard()
{
#ifdef HAVE_D3D11VA
    // More surfaces in one window than one hand-over worker takes, each with its own picture.
    constexpr int COUNT = 5;
    QWidget window;
    window.resize(COUNT * 160, 90);
    QList<VideoSurface *> surfaces;
    for (int i = 0; i < COUNT; i++) {
        auto *surface = new VideoSurface(&window);
        surface->setGeometry(i * 160, 0, 160, 90);
        surfaces.append(surface);
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (VideoSurface *surface : surfaces)
        shot(*surface);
    if (!surfaces.first()->sink()->acceptsHardwareFrames(AV_PIX_FMT_D3D11))
        QSKIP("This surface cannot draw a Direct3D picture where it is");

    // Alternately white and red.
    for (int i = 0; i < COUNT; i++) {
        AVFrame *frame = i % 2 == 0 ? solidFrameOnCard(640, 360, 235, 128, 128) : solidFrameOnCard(640, 360, 81, 90, 240);
        if (!frame)
            QSKIP("No Direct3D 11 video device");
        present(*surfaces[i], frame);
    }
    for (VideoSurface *surface : surfaces)
        QTRY_VERIFY(surface->drawsFromCard());

    for (int i = 0; i < COUNT; i++) {
        const QImage image = shot(*surfaces[i]);
        const QRgb pixel = image.pixel(image.width() / 2, image.height() / 2);
        QVERIFY2(isNear(pixel, 255, i % 2 == 0 ? 255 : 0, i % 2 == 0 ? 255 : 0), qPrintable(QStringLiteral("surface %1").arg(i)));
    }

    // One that is hidden gets no pictures made, and catches up when it is shown again.
    surfaces[1]->hide();
    present(*surfaces[1], solidFrameOnCard(640, 360, 235, 128, 128));
    surfaces[1]->show();
    QTRY_VERIFY(isNear(shot(*surfaces[1]).pixel(40, 40), 255, 255, 255));
#else
    QSKIP("Direct3D 11 only");
#endif
}

void TestVideoSurface::pictureSurvivesAMoveToAnotherWindow()
{
    // As when a tile is popped out of the call view and put back: the surface gets a new
    // window, and with the graphics card a new context, but keeps what it was showing.
    QWidget first;
    first.resize(320, 180);
    auto *surface = new VideoSurface(&first);
    surface->setGeometry(0, 0, 320, 180);
    first.show();
    QVERIFY(QTest::qWaitForWindowExposed(&first));
    present(*surface, solidFrame(640, 360, 235, 128, 128));
    QImage image = shot(*surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

    QWidget second;
    second.resize(320, 180);
    surface->setParent(&second);
    surface->setGeometry(0, 0, 320, 180);
    surface->show();
    second.show();
    QVERIFY(QTest::qWaitForWindowExposed(&second));
    image = shot(*surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

    // And a new frame is drawn there as it would have been.
    present(*surface, solidFrame(1280, 720, 63, 102, 240));
    image = shot(*surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

#ifdef HAVE_D3D11VA
    if (surface->sink()->acceptsHardwareFrames(AV_PIX_FMT_D3D11)) {
        if (AVFrame *frame = solidFrameOnCard(640, 360, 235, 128, 128)) {
            present(*surface, frame);
            QTRY_VERIFY(surface->drawsFromCard());

            // Back to the first window with a picture from the card up.
            surface->setParent(&first);
            surface->setGeometry(0, 0, 320, 180);
            surface->show();
            QTRY_VERIFY(isNear(shot(*surface).pixel(160, 90), 255, 255, 255));
        }
    }
#endif
}

void TestVideoSurface::clickAndDoubleClickAreReported()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    QSignalSpy clicks(&surface, &VideoSurface::clicked);
    QSignalSpy doubleClicks(&surface, &VideoSurface::doubleClicked);
    QTest::mouseClick(&surface, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), 1);
    QTest::mouseClick(&surface, Qt::RightButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), 1);
    QTest::mouseDClick(&surface, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(doubleClicks.count(), 1);

    if (!surface.drawsWithGpu())
        return;

    // With the graphics card the picture is a window over the surface, which gets the mouse
    // first and has to hand it on.
    shot(surface);
    QWindow *layer = nullptr;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        if (window->parent() == surface.windowHandle())
            layer = window;
    }
    QVERIFY(layer);

    surface.setContextMenuPolicy(Qt::CustomContextMenu);
    QSignalSpy menus(&surface, &QWidget::customContextMenuRequested);
    const int clicksBefore = clicks.count();
    QTest::mouseClick(layer, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), clicksBefore + 1);
    QCOMPARE(menus.count(), 0);
    QTest::mouseClick(layer, Qt::RightButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), clicksBefore + 1);
    QCOMPARE(menus.count(), 1);
    QCOMPARE(menus.first().first().toPoint(), QPoint(100, 60));
    QTest::mouseDClick(layer, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(doubleClicks.count(), 2);
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

void TestVideoSurface::speakingRingIsDrawn()
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
    image = shot(surface);
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0, 0, 0));
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
    large.setCaption(QStringLiteral("caption text"));
    QImage image = shot(large);
    // Inside the backing but clear of the text, which is itself white.
    QVERIFY(!isNear(image.pixel(5, 5), 255, 255, 255));

    VideoSurface small;
    small.resize(160, 90);
    small.show();
    QVERIFY(QTest::qWaitForWindowExposed(&small));
    present(small, solidFrame(640, 360, 235, 128, 128));
    small.setStatsText(stats);
    image = shot(small);
    QVERIFY(isNear(image.pixel(5, 5), 255, 255, 255));
}

QTEST_MAIN(TestVideoSurface)
#include "tst_VideoSurface.moc"
