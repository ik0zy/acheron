// Tests for the software video surface: a decoded frame handed to its sink ends up on screen in
// the right place, at the right size and in the right colours, and the surface reports how
// large the picture is drawn.

#include "Core/Media/HardwareDevices.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QLibrary>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QSignalSpy>
#include <QWindow>
#include <QTest>

#include <algorithm>
#include <array>
#include <ctime>

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
#define CARD_FORMAT AV_PIX_FMT_D3D11
#elif defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include <libavutil/hwcontext_drm.h>
#define HAVE_VAAPI 1
#define CARD_FORMAT AV_PIX_FMT_VAAPI
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

#ifdef CARD_FORMAT
// The same picture in memory, laid out as a hardware decoder's are.
AVFrame *solidNv12Frame(int width, int height, uint8_t y, uint8_t cb, uint8_t cr)
{
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
    return memory;
}
#endif

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

    AVFrame *memory = solidNv12Frame(width, height, y, cb, cr);
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

QImage shot(VideoSurface &surface);

// The windows a surface has put inside its own. The layer for the graphics card is the only
// one it ever puts there.
int layersOf(const VideoSurface &surface)
{
    int layers = 0;
    const QWindowList windows = QGuiApplication::allWindows();
    for (const QWindow *window : windows)
        layers += surface.windowHandle() && window->parent() == surface.windowHandle() ? 1 : 0;
    return layers;
}

// A surface has its layer for the graphics card, and knows what that layer can draw, once it
// has had a picture to draw there.
QImage drawnOnce(VideoSurface &surface)
{
    present(surface, solidFrame(64, 36, 16, 128, 128));
    return shot(surface);
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

#ifdef HAVE_VAAPI
// drm_fourcc.h
constexpr quint64 MODIFIER_LINEAR = 0;
constexpr quint64 MODIFIER_INVALID = 0x00ffffffffffffffULL;

// The same picture ten bits deep, which no hardware decoder gives in eight.
AVFrame *solidP010Frame(int width, int height, uint8_t y, uint8_t cb, uint8_t cr)
{
    AVFrame *memory = av_frame_alloc();
    memory->format = AV_PIX_FMT_P010;
    memory->width = width;
    memory->height = height;
    av_frame_get_buffer(memory, 0);
    for (int row = 0; row < height; row++) {
        auto *line = reinterpret_cast<uint16_t *>(memory->data[0] + row * memory->linesize[0]);
        std::fill(line, line + width, static_cast<uint16_t>(y << 8));
    }
    for (int row = 0; row < height / 2; row++) {
        auto *pair = reinterpret_cast<uint16_t *>(memory->data[1] + row * memory->linesize[1]);
        for (int column = 0; column < width / 2; column++) {
            *pair++ = static_cast<uint16_t>(cb << 8);
            *pair++ = static_cast<uint16_t>(cr << 8);
        }
    }
    return memory;
}

// The surfaces a VAAPI decoder would have on the card behind a render node: so many, or with
// none given as many as are asked for. Null where that card has no VAAPI, or none so laid out.
AVBufferRef *vaapiPool(const QByteArray &node, int width, int height, int surfaces = 0, AVPixelFormat layout = AV_PIX_FMT_NV12)
{
    AVBufferRef *device = nullptr;
    if (!Acheron::Core::Media::hardwareDeviceCanBeAskedFor(AV_HWDEVICE_TYPE_VAAPI) || av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.constData(), nullptr, 0) < 0)
        return nullptr;
    AVBufferRef *pool = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);
    auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = layout;
    frames->width = width;
    frames->height = height;
    frames->initial_pool_size = surfaces;
    if (av_hwframe_ctx_init(pool) < 0)
        av_buffer_unref(&pool);
    return pool;
}

// The picture, which is freed, in one of the pool's surfaces. Null when the pool has none left.
AVFrame *onCard(AVBufferRef *pool, AVFrame *memory)
{
    AVFrame *frame = av_frame_alloc();
    if (av_hwframe_get_buffer(pool, frame, 0) < 0 || av_hwframe_transfer_data(frame, memory, 0) < 0)
        av_frame_free(&frame);
    av_frame_free(&memory);
    return frame;
}

AVFrame *solidFrameFrom(AVBufferRef *pool, uint8_t y, uint8_t cb, uint8_t cr)
{
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(pool->data);
    return onCard(pool, solidNv12Frame(frames->width, frames->height, y, cb, cr));
}

// A picture in the top left of a frame that is white beyond it, as a decoder's is in a surface
// of whole blocks: brighter to the right and bluer downwards, so that a misplaced row shows.
AVFrame *rampNv12Frame(QSize picture, QSize coded)
{
    AVFrame *memory = solidNv12Frame(coded.width(), coded.height(), 235, 128, 128);
    for (int row = 0; row < picture.height(); row++) {
        for (int column = 0; column < picture.width(); column++)
            memory->data[0][row * memory->linesize[0] + column] = static_cast<uint8_t>(16 + 219 * column / picture.width());
    }
    for (int row = 0; row < picture.height() / 2; row++) {
        for (int column = 0; column < picture.width() / 2; column++)
            memory->data[1][row * memory->linesize[1] + 2 * column] = static_cast<uint8_t>(16 + 224 * row / (picture.height() / 2));
    }
    return memory;
}

AVFrame *solidFrameOnNode(const QByteArray &node, int width, int height, uint8_t y, uint8_t cb, uint8_t cr)
{
    AVBufferRef *pool = vaapiPool(node, width, height);
    if (!pool)
        return nullptr;
    AVFrame *frame = solidFrameFrom(pool, y, cb, cr);
    av_buffer_unref(&pool);
    return frame;
}

QList<QByteArray> renderNodes()
{
    QList<QByteArray> nodes;
    const QStringList names = QDir(QStringLiteral("/dev/dri")).entryList({ QStringLiteral("renderD*") }, QDir::System);
    for (const QString &name : names)
        nodes.append("/dev/dri/" + name.toLatin1());
    return nodes;
}

// What OpenGL here runs on, when that is EGL: whether it can be told how a card orders its
// memory, and the render node of the card it draws with.
struct EglHere
{
    bool present = false;
    bool takesModifiers = false;
    QByteArray renderNode;
};

const EglHere &eglHere()
{
    static const EglHere here = [] {
        // EGL/egl.h and EGL/eglext.h
        constexpr int EXTENSIONS = 0x3055;
        constexpr int DEVICE = 0x322C;
        constexpr int DRM_RENDER_NODE_FILE = 0x3377;

        EglHere found;
        QOffscreenSurface offscreen;
        offscreen.create();
        QOpenGLContext context;
        const auto currentDisplay = reinterpret_cast<void *(*)()>(QLibrary::resolve(QStringLiteral("EGL"), 1, "eglGetCurrentDisplay"));
        void *display = currentDisplay && context.create() && context.makeCurrent(&offscreen) ? currentDisplay() : nullptr;
        if (!display)
            return found;
        found.present = true;
        const auto queryString = reinterpret_cast<const char *(*)(void *, int)>(context.getProcAddress("eglQueryString"));
        found.takesModifiers = strstr(queryString(display, EXTENSIONS), "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        const auto displayAttribute = reinterpret_cast<unsigned int (*)(void *, int, qintptr *)>(context.getProcAddress("eglQueryDisplayAttribEXT"));
        const auto deviceString = reinterpret_cast<const char *(*)(void *, int)>(context.getProcAddress("eglQueryDeviceStringEXT"));
        qintptr device = 0;
        if (displayAttribute && deviceString && displayAttribute(display, DEVICE, &device) && device)
            found.renderNode = deviceString(reinterpret_cast<void *>(device), DRM_RENDER_NODE_FILE);
        return found;
    }();
    return here;
}

// How the card orders the memory of the frame, as it says when handing it over.
quint64 modifierOf(const AVFrame *frame)
{
    AVFrame *exported = av_frame_alloc();
    exported->format = AV_PIX_FMT_DRM_PRIME;
    quint64 modifier = MODIFIER_INVALID;
    if (av_hwframe_map(exported, frame, AV_HWFRAME_MAP_READ) >= 0)
        modifier = reinterpret_cast<const AVDRMFrameDescriptor *>(exported->data[0])->objects[0].format_modifier;
    av_frame_free(&exported);
    return modifier;
}

// The render node of the card OpenGL draws with here, when a surface does draw its pictures
// where they are. Empty when it does not.
QByteArray drawingNode()
{
    static const QByteArray found = [] {
        const QByteArray node = eglHere().renderNode;
        VideoSurface surface;
        surface.resize(160, 90);
        surface.show();
        AVFrame *frame = node.isEmpty() ? nullptr : solidFrameOnNode(node, 640, 360, 235, 128, 128);
        if (!frame || !QTest::qWaitForWindowExposed(&surface)) {
            av_frame_free(&frame);
            return QByteArray();
        }
        shot(surface);
        present(surface, frame);
        shot(surface);
        return surface.drawsFromCard() ? node : QByteArray();
    }();
    return found;
}

// The same picture as a hardware decoder hands it over: in one of its surfaces on the graphics
// card. Null on a machine with no VAAPI on the card that draws.
AVFrame *solidFrameOnCard(int width, int height, uint8_t y, uint8_t cb, uint8_t cr)
{
    return drawingNode().isEmpty() ? nullptr : solidFrameOnNode(drawingNode(), width, height, y, cb, cr);
}
#endif

} // namespace

class TestVideoSurface : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void emptySurfaceIsBlack();
    void frameFillsAMatchingSurface();
    void narrowerFrameIsCentredBetweenBars();
    void coloursFollowTheVideoStandard();
    void memoryEglCannotBeToldTheOrderOfIsReadBack();
    void frameOnTheGraphicsCardIsDrawnFromThere();
    void manySurfacesDrawFromTheGraphicsCard();
    void pictureFromTheCardIsThePictureInMemory();
    void tenBitPictureFromTheCardIsReadBack();
    void cardSurfaceIsKeptFromTheDecoderWhileItIsDrawn();
    void pictureSurvivesAMoveToAnotherWindow();
    void clickAndDoubleClickAreReported();
    void geometryIsReported();
    void resizeRedrawsTheLastFrame();
    void clearForgetsThePicture();
    void messagesDoNotDisturbThePicture();
    void avatarStandsInUntilThereIsAPicture();
    void speakingRingIsDrawn();
    void aSurfaceHasItsLayerOnlyWhileItHasAPicture();
    void aSurfaceLongOffTheScreenGivesUpItsLayerAndKeepsItsPicture();
    void statisticsAreLeftOffASmallTile();
    void whatShowingAFrameCosts_data();
    void whatShowingAFrameCosts();
    // Last: a card found not to hand its pictures over is not asked again while the program runs.
    void pictureFromAnotherCardIsReadBack();
};

// Which card the surfaces draw with, for whoever reads the results.
void TestVideoSurface::initTestCase()
{
    QOffscreenSurface offscreen;
    offscreen.create();
    QOpenGLContext context;
    if (!context.create() || !context.makeCurrent(&offscreen))
        return;
    qInfo().noquote() << QStringLiteral("OpenGL draws with %1").arg(QString::fromLatin1(reinterpret_cast<const char *>(context.functions()->glGetString(GL_RENDERER))));
}

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

void TestVideoSurface::memoryEglCannotBeToldTheOrderOfIsReadBack()
{
#ifdef HAVE_VAAPI
    // Before anything else is drawn from the card: what EGL makes of it is settled once.
    if (!eglHere().present || eglHere().takesModifiers)
        QSKIP("Only for an EGL that cannot be told how a card orders its memory");
    AVFrame *frame = eglHere().renderNode.isEmpty() ? nullptr : solidFrameOnNode(eglHere().renderNode, 1280, 720, 63, 102, 240);
    if (!frame)
        QSKIP("No video device on the card that draws");
    const quint64 modifier = modifierOf(frame);

    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    drawnOnce(surface);
    const bool offered = surface.sink()->acceptsHardwareFrames(CARD_FORMAT);
    present(surface, frame);
    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    // Rows one after another need no telling of, and are drawn where they are.
    QCOMPARE(surface.drawsFromCard(), offered && (modifier == MODIFIER_LINEAR || modifier == MODIFIER_INVALID));
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::frameOnTheGraphicsCardIsDrawnFromThere()
{
#ifdef CARD_FORMAT
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));

    // The surface knows whether it can once it has drawn.
    drawnOnce(surface);
    if (!surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP("This surface cannot draw the card's picture where it is");
    QVERIFY(!surface.sink()->acceptsHardwareFrames(AV_PIX_FMT_DXVA2_VLD));

    // Red in BT.709, as an HD frame is read.
    AVFrame *frame = solidFrameOnCard(1280, 720, 63, 102, 240);
    if (!frame)
        QSKIP("No video device whose pictures this surface can draw");
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
    QSKIP("Direct3D 11 and VAAPI only");
#endif
}

void TestVideoSurface::manySurfacesDrawFromTheGraphicsCard()
{
#ifdef CARD_FORMAT
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
        drawnOnce(*surface);
    if (!surfaces.first()->sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP("This surface cannot draw the card's picture where it is");

    // Alternately white and red.
    for (int i = 0; i < COUNT; i++) {
        AVFrame *frame = i % 2 == 0 ? solidFrameOnCard(640, 360, 235, 128, 128) : solidFrameOnCard(640, 360, 81, 90, 240);
        if (!frame)
            QSKIP("No video device whose pictures this surface can draw");
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
    QSKIP("Direct3D 11 and VAAPI only");
#endif
}

void TestVideoSurface::pictureFromTheCardIsThePictureInMemory()
{
#ifdef HAVE_VAAPI
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    drawnOnce(surface);
    if (!surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP("This surface cannot draw the card's picture where it is");
    if (drawingNode().isEmpty())
        QSKIP("No video device whose pictures this surface can draw");

    // A picture that does not fill its surface, of a size that a card pads the rows of.
    const QSize picture(500, 282);
    const QSize coded(512, 288);
    AVFrame *memory = rampNv12Frame(picture, coded);
    AVBufferRef *pool = vaapiPool(drawingNode(), coded.width(), coded.height());
    QVERIFY(pool);
    AVFrame *card = onCard(pool, av_frame_clone(memory));
    av_buffer_unref(&pool);
    QVERIFY(card);

    memory->width = picture.width();
    memory->height = picture.height();
    present(surface, memory);
    const QImage fromMemory = shot(surface);
    QVERIFY(!surface.drawsFromCard());
    QVERIFY(qBlue(fromMemory.pixel(160, 170)) > qBlue(fromMemory.pixel(160, 10)) + 100);
    QVERIFY(qGreen(fromMemory.pixel(310, 90)) > qGreen(fromMemory.pixel(10, 90)) + 100);

    card->width = picture.width();
    card->height = picture.height();
    present(surface, card);
    const QImage fromCard = shot(surface);
    QVERIFY(surface.drawsFromCard());
    QCOMPARE(fromCard.size(), fromMemory.size());
    for (int y = 0; y < fromCard.height(); y++) {
        for (int x = 0; x < fromCard.width(); x++) {
            const QRgb expected = fromMemory.pixel(x, y);
            QVERIFY2(isNear(fromCard.pixel(x, y), qRed(expected), qGreen(expected), qBlue(expected)), qPrintable(QStringLiteral("at %1, %2").arg(x).arg(y)));
        }
    }
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::tenBitPictureFromTheCardIsReadBack()
{
#ifdef HAVE_VAAPI
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    drawnOnce(surface);
    if (!surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP("This surface cannot draw the card's picture where it is");
    if (drawingNode().isEmpty())
        QSKIP("No video device whose pictures this surface can draw");
    AVBufferRef *pool = vaapiPool(drawingNode(), 1280, 720, 0, AV_PIX_FMT_P010);
    if (!pool)
        QSKIP("No ten-bit surfaces on this card");

    // Not a picture the surface draws where it is, and shown all the same.
    AVFrame *frame = onCard(pool, solidP010Frame(1280, 720, 63, 102, 240));
    av_buffer_unref(&pool);
    QVERIFY(frame);
    present(surface, frame);
    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QVERIFY(!surface.drawsFromCard());
    QVERIFY(!surface.sink()->acceptsHardwareFrames(CARD_FORMAT));

    // That was this stream's doing and not the card's: another surface is offered its pictures.
    VideoSurface other;
    other.resize(320, 180);
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    drawnOnce(other);
    QVERIFY(other.sink()->acceptsHardwareFrames(CARD_FORMAT));
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::cardSurfaceIsKeptFromTheDecoderWhileItIsDrawn()
{
#ifdef HAVE_VAAPI
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    drawnOnce(surface);
    if (!surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP("This surface cannot draw the card's picture where it is");
    if (drawingNode().isEmpty())
        QSKIP("No video device whose pictures this surface can draw");

    // A decoder with two surfaces, which decodes into whichever nobody holds.
    AVBufferRef *pool = vaapiPool(drawingNode(), 640, 360, 2);
    QVERIFY(pool);
    present(surface, solidFrameFrom(pool, 81, 90, 240));
    QImage image = shot(surface);
    QVERIFY(surface.drawsFromCard());
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

    // The next frame has come but is not drawn yet: the one on screen is still drawn from.
    AVFrame *white = solidFrameFrom(pool, 235, 128, 128);
    QVERIFY(white);
    surface.sink()->present(white);
    av_frame_free(&white);
    AVFrame *spare = av_frame_alloc();
    QVERIFY(av_hwframe_get_buffer(pool, spare, 0) < 0);

    // Once the new one is drawn, the old one goes back.
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));
    QCOMPARE(av_hwframe_get_buffer(pool, spare, 0), 0);

    // And so does the new one when the picture is gone, though not before that is drawn.
    surface.clear();
    AVFrame *other = av_frame_alloc();
    QVERIFY(av_hwframe_get_buffer(pool, other, 0) < 0);
    shot(surface);
    QCOMPARE(av_hwframe_get_buffer(pool, other, 0), 0);

    av_frame_free(&spare);
    av_frame_free(&other);
    av_buffer_unref(&pool);
#else
    QSKIP("VAAPI only");
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

#ifdef CARD_FORMAT
    if (surface->sink()->acceptsHardwareFrames(CARD_FORMAT)) {
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
    drawnOnce(surface);
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
    // Inside a window, as a tile is: the size of a window of its own is the compositor's to
    // set on Wayland, and it sets it back.
    QWidget window;
    window.resize(480, 270);
    VideoSurface surface(&window);
    surface.setGeometry(0, 0, 320, 180);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
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

    // Around a picture as around none, along every edge and nowhere else. The corners are
    // left out: the painter cuts them off and the graphics card does not.
    present(surface, solidFrame(640, 360, 235, 128, 128));
    surface.setSpeaking(true);
    image = shot(surface);
    const int right = image.width() - 1;
    const int bottom = image.height() - 1;
    for (const QPoint &onTheRing : { QPoint(0, bottom / 2), QPoint(right, bottom / 2), QPoint(right / 2, 0), QPoint(right / 2, bottom) })
        QVERIFY2(isNear(image.pixel(onTheRing), 0x23, 0xA5, 0x5A), qPrintable(QStringLiteral("%1,%2").arg(onTheRing.x()).arg(onTheRing.y())));
    const int inside = qRound(4 * image.devicePixelRatio());
    for (const QPoint &offIt : { QPoint(inside, bottom / 2), QPoint(right - inside, bottom / 2), QPoint(right / 2, inside), QPoint(right / 2, bottom - inside), QPoint(right / 2, bottom / 2) })
        QVERIFY2(isNear(image.pixel(offIt), 255, 255, 255), qPrintable(QStringLiteral("%1,%2").arg(offIt.x()).arg(offIt.y())));

    surface.setSpeaking(false);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(0, bottom / 2), 255, 255, 255));
}

void TestVideoSurface::aSurfaceHasItsLayerOnlyWhileItHasAPicture()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    if (!surface.drawsWithGpu())
        QSKIP("This surface draws without the graphics card");

    // All that a tile without video shows is drawn without one.
    QPixmap avatar(64, 64);
    avatar.fill(QColor(0, 200, 0));
    surface.setPlaceholder(avatar);
    surface.setCaption(QStringLiteral("Somebody"));
    surface.setSpeaking(true);
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0x23, 0xA5, 0x5A));
    QCOMPARE(layersOf(surface), 0);

    present(surface, solidFrame(640, 360, 235, 128, 128));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0x23, 0xA5, 0x5A));
    QCOMPARE(layersOf(surface), 1);

    surface.clear();
    QCOMPARE(layersOf(surface), 0);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));

    // And has one again for the next picture.
    present(surface, solidFrame(1280, 720, 63, 102, 240));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(layersOf(surface), 1);
}

void TestVideoSurface::aSurfaceLongOffTheScreenGivesUpItsLayerAndKeepsItsPicture()
{
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    if (!surface.drawsWithGpu())
        QSKIP("This surface draws without the graphics card");

    // Red, from the card where the surface draws from there.
    drawnOnce(surface);
    AVFrame *onCard = nullptr;
#ifdef CARD_FORMAT
    if (surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
        onCard = solidFrameOnCard(1280, 720, 63, 102, 240);
#endif
    const bool fromCard = onCard != nullptr;
    present(surface, fromCard ? onCard : solidFrame(1280, 720, 63, 102, 240));
    if (fromCard)
        QTRY_VERIFY(surface.drawsFromCard());
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(layersOf(surface), 1);

    // Not at once: a surface that is back in a moment has lost nothing.
    surface.hide();
    QTest::qWait(500);
    QCOMPARE(layersOf(surface), 1);
    QTest::qWait(VideoSurface::NOT_SHOWN_BEFORE_RELEASED_MS);
    QCOMPARE(layersOf(surface), 0);

    // The picture is still there to come back to, out of memory now.
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(layersOf(surface), 1);
    QVERIFY(!surface.drawsFromCard());
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

void TestVideoSurface::whatShowingAFrameCosts_data()
{
    QTest::addColumn<QSize>("size");
    QTest::newRow("1920x1080") << QSize(1920, 1080);
    QTest::newRow("2560x1440") << QSize(2560, 1440);
}

// A measurement: a picture the decoder left on the card, read back and uploaded as against
// drawn where it is. A small surface is read back each time, so that the card has done its part.
void TestVideoSurface::whatShowingAFrameCosts()
{
#ifdef HAVE_VAAPI
    QFETCH(QSize, size);
    VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    drawnOnce(surface);
    if (!surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP("This surface cannot draw the card's picture where it is");
    if (drawingNode().isEmpty())
        QSKIP("No video device whose pictures this surface can draw");

    AVBufferRef *pool = vaapiPool(drawingNode(), size.width(), size.height());
    QVERIFY(pool);
    std::array<AVFrame *, 4> pictures;
    for (size_t i = 0; i < pictures.size(); i++) {
        pictures[i] = solidFrameFrom(pool, static_cast<uint8_t>(60 + 40 * i), 128, 128);
        QVERIFY(pictures[i]);
    }

    constexpr int FRAMES = 300;
    for (const bool fromCard : { false, true }) {
        AVFrame *memory = av_frame_alloc();
        QElapsedTimer wall;
        wall.start();
        const std::clock_t cpuBefore = std::clock();
        for (int i = 0; i < FRAMES; i++) {
            const AVFrame *frame = pictures[static_cast<size_t>(i) % pictures.size()];
            if (!fromCard) {
                // As the decoder does for a sink that does not take the card's frames.
                av_frame_unref(memory);
                QVERIFY(av_hwframe_transfer_data(memory, frame, 0) >= 0);
                av_frame_copy_props(memory, frame);
                frame = memory;
            }
            surface.sink()->present(frame);
            QCoreApplication::processEvents();
            QVERIFY(!surface.snapshot().isNull());
            QCOMPARE(surface.drawsFromCard(), fromCard);
        }
        const double cpuMs = 1000.0 * static_cast<double>(std::clock() - cpuBefore) / CLOCKS_PER_SEC / FRAMES;
        const double wallMs = static_cast<double>(wall.nsecsElapsed()) / 1e6 / FRAMES;
        av_frame_free(&memory);
        qInfo().noquote() << QStringLiteral("%1: %2 ms and %3 ms of processor time a frame").arg(fromCard ? QStringLiteral("drawn from the card") : QStringLiteral("read back and uploaded")).arg(wallMs, 0, 'f', 3).arg(cpuMs, 0, 'f', 3);
        QVERIFY(cpuMs > 0);
    }

    for (AVFrame *picture : pictures)
        av_frame_free(&picture);
    av_buffer_unref(&pool);
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::pictureFromAnotherCardIsReadBack()
{
#ifdef HAVE_VAAPI
    // A machine with two cards can decode on the one that does not draw, whose memory the
    // other may be unable to read where it is.
    const QList<QByteArray> nodes = renderNodes();
    for (const QByteArray &node : nodes) {
        if (node == eglHere().renderNode)
            continue;
        VideoSurface surface;
        surface.resize(320, 180);
        surface.show();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        drawnOnce(surface);
        if (!surface.sink()->acceptsHardwareFrames(CARD_FORMAT))
            QSKIP("This surface cannot draw the card's picture where it is");
        AVFrame *frame = solidFrameOnNode(node, 1280, 720, 63, 102, 240);
        if (!frame)
            continue;

        // Drawn either way, and the first frame already.
        present(surface, frame);
        QImage image = shot(surface);
        QVERIFY2(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0), node.constData());
        if (surface.drawsFromCard())
            continue;

        // The decoder is told to read its pictures back from now on, and those are drawn.
        QVERIFY(!surface.sink()->acceptsHardwareFrames(CARD_FORMAT));
        present(surface, solidFrame(640, 360, 235, 128, 128));
        image = shot(surface);
        QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

        // Nor does a later surface ask for what this one could not draw.
        VideoSurface later;
        later.resize(320, 180);
        later.show();
        QVERIFY(QTest::qWaitForWindowExposed(&later));
        drawnOnce(later);
        QVERIFY(!later.sink()->acceptsHardwareFrames(CARD_FORMAT));
        return;
    }
    QSKIP("No second graphics card whose pictures the first cannot draw where they are");
#else
    QSKIP("VAAPI only");
#endif
}

// On a real screen the windows of the tests are for nobody to look at: they open on a screen
// that is not the first one, where there is one, and take the keyboard from nobody.
class OutOfTheWay : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *window = qobject_cast<QWidget *>(watched);
        if (event->type() != QEvent::Polish || !window || !window->isWindow())
            return false;

        window->setAttribute(Qt::WA_ShowWithoutActivating);
        const QList<QScreen *> screens = QGuiApplication::screens();
        const auto another = std::find_if(screens.begin(), screens.end(), [](const QScreen *screen) { return screen != QGuiApplication::primaryScreen(); });
        if (another != screens.end())
            window->move((*another)->availableGeometry().topLeft() + QPoint(48, 48));
        return false;
    }
};

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    OutOfTheWay outOfTheWay;
    app.installEventFilter(&outOfTheWay);
    TestVideoSurface test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_VideoSurface.moc"
