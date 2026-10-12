#include "Core/Video/ScreenCapture.hpp"

#include "Core/Logging.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QOperatingSystemVersion>
#include <QSettings>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Capture is ffmpeg's desktop duplication and graphics capture sources, which are in libavfilter
// and Windows only.
#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_SCREEN_CAPTURE
#endif

#ifdef ACHERON_SCREEN_CAPTURE
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

namespace Acheron {
namespace Core {
namespace Video {

namespace {

const char *const CAPTURE_METHOD_KEY = "video/capture_method";
const QLatin1String AUTO_METHOD("auto");
const QLatin1String GRAPHICS_CAPTURE_METHOD("graphics_capture");
const QLatin1String DESKTOP_DUPLICATION_METHOD("desktop_duplication");

} // namespace

QSize encodeSizeFor(const QSize &source, int maxHeight)
{
    QSize size = source;
    const QSize bound(maxHeight * 16 / 9, maxHeight);
    if (maxHeight > 0 && (source.width() > bound.width() || source.height() > bound.height()))
        size = source.scaled(bound, Qt::KeepAspectRatio);
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

CaptureMethod captureMethod()
{
    const QString stored = QSettings().value(CAPTURE_METHOD_KEY).toString();
    if (stored == GRAPHICS_CAPTURE_METHOD)
        return CaptureMethod::GraphicsCapture;
    if (stored == DESKTOP_DUPLICATION_METHOD)
        return CaptureMethod::DesktopDuplication;
    return CaptureMethod::Auto;
}

void setCaptureMethod(CaptureMethod method)
{
    QLatin1String stored = AUTO_METHOD;
    if (method == CaptureMethod::GraphicsCapture)
        stored = GRAPHICS_CAPTURE_METHOD;
    else if (method == CaptureMethod::DesktopDuplication)
        stored = DESKTOP_DUPLICATION_METHOD;
    QSettings().setValue(CAPTURE_METHOD_KEY, QString(stored));
}

bool stillPictureIsDue(qint64 msSinceLastPicture, bool keyFrameWaitedFor, int framerate)
{
    return msSinceLastPicture >= STILL_PICTURE_INTERVAL_MS || (keyFrameWaitedFor && msSinceLastPicture >= 1000 / std::max(1, framerate));
}

bool windowIsCovered(const QList<StackedWindow> &topDown, int target)
{
    if (target < 0 || target >= topDown.size())
        return true;
    const StackedWindow &window = topDown[target];
    if (window.bounds.isEmpty() || window.minimised || window.cloaked)
        return true;

    for (int i = 0; i < target; i++) {
        const StackedWindow &above = topDown[i];
        if (!above.visible || above.minimised)
            continue;
        if (above.livePreview)
            return true;
        if (above.overlay || above.cloaked || above.tabProxy)
            continue;
        if (above.bounds.intersects(window.bounds))
            return true;
    }
    return false;
}

#ifdef ACHERON_SCREEN_CAPTURE

namespace {

using Clock = std::chrono::steady_clock;

const QLatin1String SCREEN_ID_PREFIX("screen:");
const QLatin1String WINDOW_ID_PREFIX("window:");
constexpr int HD_HEIGHT = 720;
constexpr int STALLED_WAIT_MS = 50;
// How long a read of graphics capture waits past the time a picture was due before it says
// that nothing new has come.
constexpr int NOTHING_NEW_WAIT_MS = 25;
// A window smaller than this is sent with black around it: encoders on graphics cards turn
// down pictures much smaller.
constexpr QSize SMALLEST_WINDOW_PICTURE(160, 120);
// How far a window may reach past the edge of its screen and still be cut out of it, as the
// official client has it.
constexpr int REACH_PAST_SCREEN = 15;
constexpr int THUMBNAIL_DRAWING_LIMIT_MS = 500;

// DwmGetWindowAttribute's DWMWA_EXTENDED_FRAME_BOUNDS and DWMWA_CLOAKED, and PrintWindow's
// PW_RENDERFULLCONTENT, which the headers keep from a build that also targets Windows 7.
constexpr DWORD FRAME_BOUNDS_ATTRIBUTE = 9;
constexpr DWORD CLOAKED_ATTRIBUTE = 14;
constexpr UINT RENDER_FULL_CONTENT = 2;

// Whether scale_d3d11 was found not to work on this machine. It asks the card for an array of
// NV12 render targets, which a Radeon RX 7800 XT for one refuses to make.
std::atomic<bool> cardScalingRuledOut{ false };
// Set when graphics capture turned out to be missing something only a capture finds out.
std::atomic<bool> graphicsCaptureRuledOut{ false };

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

// For as long as one lives, its thread measures the desktop the way it asked to.
class ThreadScaling
{
public:
    explicit ThreadScaling(DPI_AWARENESS_CONTEXT wanted)
    {
        if (setAwareness && wanted)
            previous = setAwareness(wanted);
    }
    ~ThreadScaling()
    {
        if (previous)
            setAwareness(previous);
    }

    ThreadScaling(const ThreadScaling &) = delete;
    ThreadScaling &operator=(const ThreadScaling &) = delete;

    // How `window` measures, which is how it draws itself.
    [[nodiscard]] static DPI_AWARENESS_CONTEXT of(HWND window) { return windowAwareness ? windowAwareness(window) : nullptr; }

private:
    // Neither is there before Windows 10 1607.
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    using WindowAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(HWND);
    static inline const SetAwareness setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    static inline const WindowAwareness windowAwareness = reinterpret_cast<WindowAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext")));
    DPI_AWARENESS_CONTEXT previous = nullptr;
};

// Windows scales the coordinates it gives a thread that has not said it understands display
// scaling, and a thread of a program without a window has not.
class UnscaledCoordinates : public ThreadScaling
{
public:
    UnscaledCoordinates() : ThreadScaling(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) {}
};

struct Monitor
{
    int adapter = 0;
    // Its place among the adapter's outputs, attached to the desktop or not, which is how
    // ddagrab counts them.
    int output = 0;
    HMONITOR handle = nullptr;
    QRect geometry;
    // Turned a quarter, in which case Windows hands its picture over lying on its side.
    bool onItsSide = false;
    bool turned = false;
};

std::vector<Monitor> attachedMonitors()
{
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    static const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));

    std::vector<Monitor> found;
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return found;

    IDXGIAdapter *adapter = nullptr;
    for (UINT adapterIndex = 0; SUCCEEDED(factory->EnumAdapters(adapterIndex, &adapter)); adapterIndex++) {
        IDXGIOutput *output = nullptr;
        for (UINT outputIndex = 0; SUCCEEDED(adapter->EnumOutputs(outputIndex, &output)); outputIndex++) {
            DXGI_OUTPUT_DESC description;
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            // The mode is in the screen's own pixels whatever the calling thread understands,
            // which the output's desktop coordinates are not.
            if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop && EnumDisplaySettingsW(description.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                Monitor monitor;
                monitor.adapter = static_cast<int>(adapterIndex);
                monitor.output = static_cast<int>(outputIndex);
                monitor.handle = description.Monitor;
                monitor.geometry = QRect(mode.dmPosition.x, mode.dmPosition.y, static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight));
                monitor.onItsSide = description.Rotation == DXGI_MODE_ROTATION_ROTATE90 || description.Rotation == DXGI_MODE_ROTATION_ROTATE270;
                monitor.turned = monitor.onItsSide || description.Rotation == DXGI_MODE_ROTATION_ROTATE180;
                found.push_back(monitor);
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

const Monitor *numberedMonitor(const std::vector<Monitor> &monitors, const QString &sourceId)
{
    bool numbered = false;
    const int number = sourceId.mid(SCREEN_ID_PREFIX.size()).toInt(&numbered);
    const bool listed = sourceId.startsWith(SCREEN_ID_PREFIX) && numbered && number >= 0 && number < static_cast<int>(monitors.size());
    return listed ? &monitors[static_cast<size_t>(number)] : nullptr;
}

QString describeError(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return QString::fromUtf8(text);
}

QSize evenSize(const QSize &size)
{
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

QRect fromNative(const RECT &rect)
{
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

HWND windowOf(const QString &sourceId)
{
    bool numbered = false;
    const qulonglong handle = sourceId.mid(WINDOW_ID_PREFIX.size()).toULongLong(&numbered);
    return sourceId.startsWith(WINDOW_ID_PREFIX) && numbered ? reinterpret_cast<HWND>(static_cast<quintptr>(handle)) : nullptr;
}

HRESULT windowAttribute(HWND window, DWORD attribute, void *value, DWORD size)
{
    using GetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static const auto getAttribute = reinterpret_cast<GetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
    return getAttribute ? getAttribute(window, attribute, value, size) : E_INVALIDARG;
}

// Kept from the screen by the program (1) or by the shell (2), as the official client counts
// it. A Windows that does not know the question calls the argument invalid, and has no cloaks.
bool isCloaked(HWND window)
{
    DWORD cloak = 0;
    const HRESULT asked = windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak));
    return SUCCEEDED(asked) ? cloak == 1 || cloak == 2 : asked != E_INVALIDARG;
}

// The window as it shows on the screen. GetWindowRect also counts the invisible borders
// Windows 10 puts around a window that can be resized.
QRect visibleBounds(HWND window)
{
    RECT bounds = {};
    if (FAILED(windowAttribute(window, FRAME_BOUNDS_ATTRIBUTE, &bounds, sizeof(bounds))) || IsRectEmpty(&bounds)) {
        if (!GetWindowRect(window, &bounds))
            return {};
    }
    return fromNative(bounds);
}

// The inside of the window's frame, which is what is shared of it.
QRect clientOnScreen(HWND window)
{
    RECT client = {};
    POINT corner = {};
    if (GetClientRect(window, &client) && ClientToScreen(window, &corner))
        return QRect(corner.x, corner.y, client.right, client.bottom);
    return GetWindowRect(window, &client) ? fromNative(client) : QRect();
}

bool hasClass(HWND window, const wchar_t *name)
{
    wchar_t found[64] = {};
    return GetClassNameW(window, found, 64) > 0 && wcscmp(found, name) == 0;
}

struct WindowStack
{
    HWND target = nullptr;
    HWND foreground = nullptr;
    QRect targetClient;
    QList<StackedWindow> topDown;
    bool reachedTarget = false;
    // Every window EnumWindows lists, below the target as well, to tell the shell's from.
    std::vector<HWND> ordinary;
};

BOOL CALLBACK stackDownToTarget(HWND window, LPARAM parameter)
{
    auto *stack = reinterpret_cast<WindowStack *>(parameter);
    stack->ordinary.push_back(window);
    if (stack->reachedTarget)
        return TRUE;

    StackedWindow each;
    each.visible = IsWindowVisible(window);
    each.minimised = IsIconic(window);
    if (window == stack->target) {
        each.bounds = stack->targetClient;
        each.cloaked = isCloaked(window);
        stack->topDown.append(each);
        stack->reachedTarget = true;
        return TRUE;
    }

    if (each.visible && !each.minimised) {
        const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        each.bounds = visibleBounds(window);
        each.cloaked = isCloaked(window);
        // A window that holds the foreground has come forward, whatever its styles say.
        each.overlay = (extended & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0 && window != stack->foreground;
        each.tabProxy = hasClass(window, L"Windows.Internal.Shell.TabProxyWindow");
        each.livePreview = (extended & WS_EX_TOPMOST) && hasClass(window, L"LivePreview");
    }
    stack->topDown.append(each);
    return TRUE;
}

// Task View, the Start menu, the notification centre and the like are windows of the shell on
// layers above every ordinary window. EnumWindows leaves them out and FindWindowEx does not,
// so they are the ones only the second finds. They exist all the time and are cloaked until
// shown, and the official client's walk never sees them: with one of them open it shares
// whatever part of it lies over the window.
bool shellSurfaceCovers(std::vector<HWND> ordinary, const QRect &client)
{
    std::sort(ordinary.begin(), ordinary.end());
    for (HWND window = FindWindowExW(nullptr, nullptr, nullptr, nullptr); window; window = FindWindowExW(nullptr, window, nullptr, nullptr)) {
        if (std::binary_search(ordinary.begin(), ordinary.end(), window) || !IsWindowVisible(window) || IsIconic(window))
            continue;

        DWORD cloak = 0;
        if (FAILED(windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak))) || cloak != 0)
            continue;
        if (visibleBounds(window).intersects(client))
            return true;
    }
    return false;
}

bool isCoveredOnScreen(HWND window, const QRect &client)
{
    WindowStack stack;
    stack.target = window;
    stack.foreground = GetForegroundWindow();
    stack.targetClient = client;
    EnumWindows(stackDownToTarget, reinterpret_cast<LPARAM>(&stack));
    if (!stack.reachedTarget || windowIsCovered(stack.topDown, static_cast<int>(stack.topDown.size()) - 1))
        return true;
    return shellSurfaceCovers(std::move(stack.ordinary), client);
}

// A window a user would call one: on the screen or minimised, with a title, and not a tool
// window, an overlay or the desktop.
bool isShareable(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() || !IsWindowVisible(window) || window == GetShellWindow() || isCloaked(window))
        return false;

    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) || ((extended & WS_EX_NOACTIVATE) && !(extended & WS_EX_APPWINDOW)))
        return false;
    return IsIconic(window) || !visibleBounds(window).isEmpty();
}

// A store app's window belongs to a host process, and the app, which is what plays its sound,
// to the process of the window inside it.
quint32 processShownIn(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (hasClass(window, L"ApplicationFrameWindow")) {
        DWORD inner = 0;
        const HWND core = FindWindowExW(window, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        if (core && GetWindowThreadProcessId(core, &inner) && inner)
            return inner;
    }
    return process;
}

BOOL CALLBACK listShareable(HWND window, LPARAM list)
{
    wchar_t title[512] = {};
    if (!isShareable(window) || GetWindowTextW(window, title, 512) <= 0)
        return TRUE;

    CaptureSource source;
    source.id = WINDOW_ID_PREFIX + QString::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(window)));
    source.kind = CaptureSource::Kind::Window;
    source.name = QString::fromWCharArray(title);
    source.processId = processShownIn(window);
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (!IsIconic(window))
        source.geometry = visibleBounds(window);
    else if (GetWindowPlacement(window, &placement))
        source.geometry = fromNative(placement.rcNormalPosition);
    reinterpret_cast<QList<CaptureSource> *>(list)->append(source);
    return TRUE;
}

// A 32-bit bitmap GDI draws into, its rows running from the top down.
class Bitmap
{
public:
    explicit Bitmap(const QSize &size) : extent(size), memory(CreateCompatibleDC(nullptr))
    {
        BITMAPINFO layout = {};
        layout.bmiHeader.biSize = sizeof(layout.bmiHeader);
        layout.bmiHeader.biWidth = size.width();
        // Negative for rows that run from the top down.
        layout.bmiHeader.biHeight = -size.height();
        layout.bmiHeader.biPlanes = 1;
        layout.bmiHeader.biBitCount = 32;
        layout.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(memory, &layout, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap)
            previous = SelectObject(memory, bitmap);
    }

    ~Bitmap()
    {
        if (bitmap) {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
    }

    Bitmap(const Bitmap &) = delete;
    Bitmap &operator=(const Bitmap &) = delete;

    [[nodiscard]] bool usable() const { return bitmap != nullptr; }
    [[nodiscard]] HDC context() const { return memory; }
    [[nodiscard]] QSize size() const { return extent; }
    [[nodiscard]] const quint32 *row(int y) const { return static_cast<const quint32 *>(bits) + static_cast<qsizetype>(y) * extent.width(); }

private:
    QSize extent;
    HDC memory;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void *bits = nullptr;
};

// `area` of what `from` holds, made no larger than `bound`.
QImage shrunk(HDC from, const QRect &area, const QSize &bound)
{
    const QSize size = area.width() > bound.width() || area.height() > bound.height() ? area.size().scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)) : area.size();
    const Bitmap reduced(size);
    if (!reduced.usable())
        return {};

    SetStretchBltMode(reduced.context(), HALFTONE);
    SetBrushOrgEx(reduced.context(), 0, 0, nullptr);
    if (!StretchBlt(reduced.context(), 0, 0, size.width(), size.height(), from, area.x(), area.y(), area.width(), area.height(), SRCCOPY))
        return {};
    GdiFlush();

    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); y++) {
        const quint32 *source = reduced.row(y);
        auto *to = reinterpret_cast<quint32 *>(image.scanLine(y));
        // GDI leaves the byte an image keeps for opacity at zero.
        for (int x = 0; x < size.width(); x++)
            to[x] = source[x] | 0xFF000000u;
    }
    return image;
}

// Has a window draw itself into a bitmap, whatever is on top of it on the screen. PrintWindow
// is called on a thread of its own because it does not come back from a program that has
// stopped answering; a drawer left waiting for one is abandoned, and goes when the call returns.
class WindowDrawer
{
public:
    static std::shared_ptr<WindowDrawer> start(HWND window, bool insideOnly)
    {
        auto drawer = std::make_shared<WindowDrawer>(window, insideOnly);
        std::thread([drawer] { drawer->run(); }).detach();
        return drawer;
    }

    WindowDrawer(HWND window, bool insideOnly) : window(window), insideOnly(insideOnly) {}

    void abandon()
    {
        {
            const std::lock_guard lock(mutex);
            abandoned = true;
        }
        changed.notify_all();
    }

    // False when the window would not draw or had not within `limit`, in which case what it
    // draws later is thrown away.
    bool draw(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex);
        if (drawing)
            return false;

        const QSize size = sizeAsTheWindowMeasures();
        if (size.isEmpty())
            return false;
        if (!bitmap || bitmap->size() != size)
            bitmap = std::make_unique<Bitmap>(size);
        if (!bitmap->usable())
            return false;

        drawing = true;
        changed.notify_all();
        changed.wait_for(lock, limit, [this] { return !drawing; });
        return !drawing && drew;
    }

    // What the last successful draw left, until the next.
    [[nodiscard]] const Bitmap &drawn() const { return *bitmap; }

    // PrintWindow leaves the pointer out. It belongs in the picture while it is over the inside
    // of the window with nothing else in between.
    void drawPointer(const QRect &client)
    {
        CURSORINFO cursor = {};
        cursor.cbSize = sizeof(cursor);
        if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING))
            return;
        const QPoint at(cursor.ptScreenPos.x, cursor.ptScreenPos.y);
        const HWND under = WindowFromPoint(cursor.ptScreenPos);
        if (!client.contains(at) || !under || GetAncestor(under, GA_ROOT) != window)
            return;

        if (cursor.hCursor != hotspotOf) {
            ICONINFO icon = {};
            hotspot = QPoint();
            if (GetIconInfo(cursor.hCursor, &icon)) {
                hotspot = QPoint(static_cast<int>(icon.xHotspot), static_cast<int>(icon.yHotspot));
                DeleteObject(icon.hbmMask);
                if (icon.hbmColor)
                    DeleteObject(icon.hbmColor);
            }
            hotspotOf = cursor.hCursor;
        }
        const QSize size = bitmap->size();
        DrawIconEx(bitmap->context(), (at.x() - client.x()) * size.width() / client.width() - hotspot.x(), (at.y() - client.y()) * size.height() / client.height() - hotspot.y(), cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        GdiFlush();
    }

private:
    // A program Windows scales for the screen measures, and draws itself, smaller than it shows.
    QSize sizeAsTheWindowMeasures() const
    {
        const ThreadScaling asTheWindow(ThreadScaling::of(window));
        RECT area = {};
        if (!(insideOnly ? GetClientRect(window, &area) : GetWindowRect(window, &area)))
            return {};
        return fromNative(area).size();
    }

    void run()
    {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [this] { return drawing || abandoned; });
            if (abandoned)
                return;

            lock.unlock();
            bool printed = false;
            {
                const ThreadScaling asTheWindow(ThreadScaling::of(window));
                printed = PrintWindow(window, bitmap->context(), RENDER_FULL_CONTENT | (insideOnly ? PW_CLIENTONLY : 0)) != FALSE;
                GdiFlush();
            }
            lock.lock();
            drew = printed;
            drawing = false;
            changed.notify_all();
        }
    }

    const HWND window;
    const bool insideOnly;
    std::mutex mutex;
    std::condition_variable changed;
    // Only the thread that is not drawing touches the bitmap.
    std::unique_ptr<Bitmap> bitmap;
    bool drawing = false;
    bool drew = false;
    bool abandoned = false;
    HCURSOR hotspotOf = nullptr;
    QPoint hotspot;
};

QImage windowThumbnail(HWND window, const QSize &bound)
{
    RECT whole = {};
    if (!window || IsIconic(window) || !GetWindowRect(window, &whole) || IsRectEmpty(&whole))
        return {};

    const std::shared_ptr<WindowDrawer> drawer = WindowDrawer::start(window, false);
    const bool drew = drawer->draw(std::chrono::milliseconds(THUMBNAIL_DRAWING_LIMIT_MS));
    drawer->abandon();
    if (!drew)
        return {};

    // What shows of the window, placed in a bitmap that may be at the smaller size a scaled
    // program draws itself at.
    const QRect outer = fromNative(whole);
    const QRect shown = visibleBounds(window).translated(-outer.topLeft()) & QRect(QPoint(0, 0), outer.size());
    const QSize drawn = drawer->drawn().size();
    const QRect inBitmap(shown.x() * drawn.width() / outer.width(), shown.y() * drawn.height() / outer.height(), shown.width() * drawn.width() / outer.width(), shown.height() * drawn.height() / outer.height());
    return inBitmap.isEmpty() ? QImage() : shrunk(drawer->drawn().context(), inBitmap, bound);
}

bool prepareBgra(AVFrame *frame, const QSize &size)
{
    if (frame->buf[0] && frame->format == AV_PIX_FMT_BGRA && frame->width == size.width() && frame->height == size.height())
        return av_frame_make_writable(frame) >= 0;

    av_frame_unref(frame);
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = size.width();
    frame->height = size.height();
    return av_frame_get_buffer(frame, 0) >= 0;
}

// The bitmap from its top left corner, with black where it does not reach.
void copyTopLeft(const Bitmap &from, AVFrame *bgra)
{
    const int columns = std::min(from.size().width(), bgra->width);
    for (int y = 0; y < bgra->height; y++) {
        uint8_t *to = bgra->data[0] + static_cast<qsizetype>(y) * bgra->linesize[0];
        const int copied = y < from.size().height() ? columns : 0;
        if (copied > 0)
            memcpy(to, from.row(y), static_cast<size_t>(copied) * 4);
        memset(to + static_cast<qsizetype>(copied) * 4, 0, static_cast<size_t>(bgra->width - copied) * 4);
    }
}

QByteArray softwareScaling(const QSize &size, bool bt601, int format)
{
    QByteArray scaling = "scale=w=" + QByteArray::number(size.width()) + ":h=" + QByteArray::number(size.height()) + ":flags=bilinear";
    if (format != AV_PIX_FMT_BGRA)
        scaling += QByteArray(":out_range=tv:out_color_matrix=") + (bt601 ? "bt601" : "bt709");
    return scaling + ",format=" + av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
}

// A graph of `chain` ending in a sink to read from. Its filters that work on a graphics card
// are given `device`.
int assemble(const QByteArray &chain, AVBufferRef *device, AVFilterGraph **graph, AVFilterContext **sink)
{
    *graph = avfilter_graph_alloc();
    int result = avfilter_graph_create_filter(sink, avfilter_get_by_name("buffersink"), "sink", nullptr, nullptr, *graph);
    if (result >= 0) {
        AVFilterInOut *inputs = nullptr;
        AVFilterInOut *outputs = nullptr;
        result = avfilter_graph_parse2(*graph, chain.constData(), &inputs, &outputs);
        if (result >= 0)
            result = outputs && !outputs->next && !inputs ? avfilter_link(outputs->filter_ctx, static_cast<unsigned>(outputs->pad_idx), *sink, 0) : AVERROR(EINVAL);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    }
    for (unsigned i = 0; device && result >= 0 && i < (*graph)->nb_filters; i++) {
        if ((*graph)->filters[i]->filter->flags & AVFILTER_FLAG_HWDEVICE)
            (*graph)->filters[i]->hw_device_ctx = av_buffer_ref(device);
    }
    if (result >= 0)
        result = avfilter_graph_config(*graph, nullptr);
    if (result < 0) {
        avfilter_graph_free(graph);
        *sink = nullptr;
    }
    return result;
}

// Scales BGRA pictures in system memory and converts them to what an encoder takes.
class PictureConverter
{
public:
    PictureConverter() = default;
    ~PictureConverter() { avfilter_graph_free(&graph); }

    PictureConverter(const PictureConverter &) = delete;
    PictureConverter &operator=(const PictureConverter &) = delete;

    // Leaves `picture` with its owner. Negative, an ffmpeg error, when it could not be converted.
    int convert(AVFrame *picture, const QSize &size, int format, bool bt601, AVFrame *into)
    {
        const QSize from(picture->width, picture->height);
        if (!graph || from != builtFrom || size != builtSize || format != builtFormat || bt601 != builtBt601) {
            avfilter_graph_free(&graph);
            source = nullptr;
            const QByteArray chain = "buffer=video_size=" + QByteArray::number(from.width()) + "x" + QByteArray::number(from.height()) + ":pix_fmt=bgra:time_base=1/90000:pixel_aspect=1/1," + softwareScaling(size, bt601, format);
            const int built = assemble(chain, nullptr, &graph, &sink);
            if (built < 0)
                return built;
            for (unsigned i = 0; i < graph->nb_filters && !source; i++) {
                if (strcmp(graph->filters[i]->filter->name, "buffer") == 0)
                    source = graph->filters[i];
            }
            builtFrom = from;
            builtSize = size;
            builtFormat = format;
            builtBt601 = bt601;
        }

        picture->pts = converted++;
        const int added = av_buffersrc_add_frame_flags(source, picture, AV_BUFFERSRC_FLAG_KEEP_REF);
        return added < 0 ? added : av_buffersink_get_frame(sink, into);
    }

private:
    AVFilterGraph *graph = nullptr;
    AVFilterContext *source = nullptr;
    AVFilterContext *sink = nullptr;
    QSize builtFrom;
    QSize builtSize;
    int builtFormat = -1;
    bool builtBt601 = false;
    int64_t converted = 0;
};

// Graphics capture, read on a thread of its own. Reading it does not come back until the
// source shows something new, which a minimised window never does, and ffmpeg has no way to
// call a read off. So a capture that is no longer wanted is only abandoned: its thread frees
// it when the read it is in returns, which is when the window is next shown or is closed.
class GraphicsCaptureSession
{
public:
    enum class Taken {
        Picture,
        Nothing,
        Ended,
    };

    // Null when the capture could not be made, with ffmpeg's reason in `error`.
    static std::shared_ptr<GraphicsCaptureSession> start(const QByteArray &chain, const QByteArray &adapter, int *error)
    {
        AVBufferRef *device = nullptr;
        *error = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, adapter.isEmpty() ? nullptr : adapter.constData(), nullptr, 0);
        if (*error < 0)
            return nullptr;

        auto session = std::make_shared<GraphicsCaptureSession>();
        *error = assemble(chain, device, &session->graph, &session->sink);
        av_buffer_unref(&device);
        if (*error < 0)
            return nullptr;
        std::thread([session] { session->run(); }).detach();
        return session;
    }

    ~GraphicsCaptureSession() { avfilter_graph_free(&graph); }

    void abandon()
    {
        const std::lock_guard lock(mutex);
        abandoned = true;
    }

    // What ffmpeg ended the capture with, or zero while it runs: the end of the stream when
    // the window was closed or hidden, an error otherwise.
    [[nodiscard]] int endedWith()
    {
        const std::lock_guard lock(mutex);
        return ended;
    }

    // The newest picture not taken yet, once it is `notBefore`. Waits for one until `giveUpAt`.
    Taken take(AVFrame *into, Clock::time_point notBefore, Clock::time_point giveUpAt)
    {
        std::unique_lock lock(mutex);
        for (;;) {
            if (ended)
                return Taken::Ended;
            const auto now = Clock::now();
            if (fresh && now >= notBefore) {
                av_frame_unref(into);
                av_frame_move_ref(into, latest.get());
                fresh = false;
                return Taken::Picture;
            }
            if (now >= giveUpAt)
                return Taken::Nothing;
            arrived.wait_until(lock, fresh ? notBefore : giveUpAt);
        }
    }

private:
    // Windows 10 hands over a picture for every refresh of the screen whatever rate it was
    // asked for, and keeps only two waiting. All are read, so that the one taken is the newest.
    void run()
    {
        const FramePointer picture(av_frame_alloc());
        for (;;) {
            const int got = av_buffersink_get_frame(sink, picture.get());
            const std::lock_guard lock(mutex);
            if (abandoned)
                break;
            if (got < 0) {
                ended = got;
                break;
            }
            av_frame_unref(latest.get());
            av_frame_move_ref(latest.get(), picture.get());
            fresh = true;
            arrived.notify_all();
        }
        arrived.notify_all();
        avfilter_graph_free(&graph);
    }

    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    std::mutex mutex;
    std::condition_variable arrived;
    FramePointer latest{ av_frame_alloc() };
    bool fresh = false;
    bool abandoned = false;
    int ended = 0;
};

// Capturing a window by its handle came with Windows 10 1903 and the filter with ffmpeg 8.1.
// Making the filter loads everything it needs of Windows, and captures nothing.
bool graphicsCaptureUsable()
{
    static const bool initialises = [] {
        const AVFilter *filter = avfilter_get_by_name("gfxcapture");
        if (!filter || QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 18362))
            return false;
        AVFilterGraph *graph = avfilter_graph_alloc();
        AVFilterContext *made = nullptr;
        const bool initialised = avfilter_graph_create_filter(&made, filter, "probe", nullptr, nullptr, graph) >= 0;
        avfilter_graph_free(&graph);
        return initialised;
    }();
    return initialises && !graphicsCaptureRuledOut;
}

bool usesGraphicsCapture(CaptureMethod method, bool forWindow)
{
    const bool preferred = method == CaptureMethod::GraphicsCapture || (method == CaptureMethod::Auto && forWindow);
    return graphicsCaptureUsable() && (preferred || !screenCaptureAvailable());
}

std::chrono::microseconds pictureInterval(int framerate)
{
    return std::chrono::microseconds(1'000'000 / std::max(1, framerate));
}

// True, after waiting a little of it, while there is time left until `retryAt`.
bool waitedForRetry(Clock::time_point retryAt)
{
    const auto now = Clock::now();
    if (now >= retryAt)
        return false;
    std::this_thread::sleep_for(std::min<Clock::duration>(retryAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)));
    return true;
}

// Frames on `device` for BGRA pictures of one size. Null when the card will not make them.
AVBufferRef *cardPictures(AVBufferRef *device, const QSize &size)
{
    AVBufferRef *pool = av_hwframe_ctx_alloc(device);
    if (!pool)
        return nullptr;
    auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_BGRA;
    frames->width = size.width();
    frames->height = size.height();
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(pool) < 0)
        av_buffer_unref(&pool);
    return pool;
}

AVD3D11VADeviceContext *deviceOf(const AVFrame *card)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWFramesContext *>(card->hw_frames_ctx->data)->device_ctx->hwctx);
}

ID3D11Resource *textureOf(const AVFrame *card)
{
    return reinterpret_cast<ID3D11Resource *>(card->data[0]);
}

UINT sliceOf(const AVFrame *card)
{
    return static_cast<UINT>(reinterpret_cast<quintptr>(card->data[1]));
}

void clearToBlack(AVFrame *card)
{
    AVD3D11VADeviceContext *device = deviceOf(card);
    ID3D11RenderTargetView *view = nullptr;
    if (FAILED(device->device->CreateRenderTargetView(textureOf(card), nullptr, &view)))
        return;
    const float black[4] = { 0, 0, 0, 1 };
    device->lock(device->lock_ctx);
    device->device_context->ClearRenderTargetView(view, black);
    device->unlock(device->lock_ctx);
    view->Release();
}

// `area` of one picture on the graphics card into another of the same card, at `at`.
void copyOnCard(const AVFrame *from, const QRect &area, AVFrame *to, const QPoint &at)
{
    AVD3D11VADeviceContext *device = deviceOf(to);
    const D3D11_BOX box = { static_cast<UINT>(area.left()), static_cast<UINT>(area.top()), 0, static_cast<UINT>(area.left() + area.width()), static_cast<UINT>(area.top() + area.height()), 1 };
    device->lock(device->lock_ctx);
    device->device_context->CopySubresourceRegion(textureOf(to), sliceOf(to), static_cast<UINT>(at.x()), static_cast<UINT>(at.y()), 0, textureOf(from), sliceOf(from), &box);
    device->unlock(device->lock_ctx);
}

} // namespace

struct ScreenCapture::Backends
{
    ~Backends()
    {
        av_buffer_unref(&pool);
        av_buffer_unref(&device);
    }

    HWND window = nullptr;
    QSize clientSize;
    Clock::time_point sizeChangedAt;
    // What is captured of a window, before any scaling. It follows the size of the window's
    // inside once that has held.
    QSize pictureSize;
    Clock::time_point pictureDue;

    std::shared_ptr<GraphicsCaptureSession> session;
    // Not before this is a capture made again that ended by itself.
    Clock::time_point remakeAt;

    // For a window by duplication: the screen it is on, the card that screen is on, and the
    // pictures of the window on that card, which both ways of capturing it fill.
    HMONITOR screenHandle = nullptr;
    std::optional<Monitor> screen;
    bool screenChanged = true;
    AVBufferRef *device = nullptr;
    int deviceAdapter = -1;
    AVBufferRef *pool = nullptr;
    std::shared_ptr<WindowDrawer> drawer;
    quint64 cutOutPictures = 0;
    quint64 drawnPictures = 0;
    std::optional<bool> lastWasCutOut;
    // Why the last picture could not be cut out of the screen, for the log.
    const char *drawnBecause = "";

    PictureConverter converter;
    PictureConverter rescaler;
    FramePointer captured{ av_frame_alloc() };
    FramePointer wholeScreen{ av_frame_alloc() };
    FramePointer bgra{ av_frame_alloc() };
    FramePointer asDrawn{ av_frame_alloc() };
};

bool screenCaptureAvailable()
{
    // Desktop duplication came with Windows 8.
    static const bool available = avfilter_get_by_name("ddagrab") && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8;
    return available;
}

bool windowCaptureAvailable()
{
    return graphicsCaptureUsable() || screenCaptureAvailable();
}

bool captureMethodUsable(CaptureMethod method)
{
    switch (method) {
    case CaptureMethod::GraphicsCapture:
        return graphicsCaptureUsable();
    case CaptureMethod::DesktopDuplication:
        return screenCaptureAvailable();
    case CaptureMethod::Auto:
        break;
    }
    return screenCaptureAvailable() || graphicsCaptureUsable();
}

QList<CaptureSource> captureSources()
{
    QList<CaptureSource> sources;
    if (!screenCaptureAvailable() && !graphicsCaptureUsable())
        return sources;

    const std::vector<Monitor> monitors = attachedMonitors();
    for (size_t i = 0; i < monitors.size(); i++) {
        CaptureSource source;
        source.id = SCREEN_ID_PREFIX + QString::number(i);
        source.name = QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "Screen %1").arg(i + 1);
        source.geometry = monitors[i].geometry;
        sources.append(source);
    }
    return sources;
}

QList<CaptureSource> captureWindows()
{
    QList<CaptureSource> windows;
    if (!windowCaptureAvailable())
        return windows;

    const UnscaledCoordinates unscaled;
    EnumWindows(listShareable, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

QImage captureThumbnail(const CaptureSource &source, const QSize &bound)
{
    if (bound.isEmpty())
        return {};

    const UnscaledCoordinates unscaled;
    if (source.kind == CaptureSource::Kind::Window)
        return windowThumbnail(windowOf(source.id), bound);
    if (source.geometry.isEmpty())
        return {};

    const HDC screen = GetDC(nullptr);
    const QImage thumbnail = shrunk(screen, source.geometry, bound);
    ReleaseDC(nullptr, screen);
    return thumbnail;
}

void restoreIfMinimised(const CaptureSource &window)
{
    const HWND handle = windowOf(window.id);
    if (handle && IsIconic(handle))
        ShowWindowAsync(handle, SW_RESTORE);
}

ScreenCapture::ScreenCapture() : backends(std::make_unique<Backends>()) {}

ScreenCapture::~ScreenCapture()
{
    tearDown();
}

void ScreenCapture::configure(const Request &wanted)
{
    tearDown();
    request = wanted;
    failingSince.reset();
    retryAt = {};
    closed = false;
    failed = false;
    failureText.clear();

    backends = std::make_unique<Backends>();
    backends->window = windowOf(request.sourceId);
    if (usesGraphicsCapture(request.method, backends->window != nullptr))
        path = Path::GraphicsCapture;
    else
        path = backends->window ? Path::WindowByDuplication : Path::Duplication;
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return path == Path::GraphicsCapture;
}

QString ScreenCapture::captureName() const
{
    switch (path) {
    case Path::GraphicsCapture:
        return QStringLiteral("gfxcapture");
    case Path::WindowByDuplication:
        return QStringLiteral("ddagrab %1 + PrintWindow %2 frames").arg(backends->cutOutPictures).arg(backends->drawnPictures);
    case Path::Duplication:
        break;
    }
    return QStringLiteral("ddagrab");
}

ScreenCapture::Status ScreenCapture::read(AVFrame *into)
{
    if (failed)
        return Status::Failed;
    if (closed)
        return Status::Closed;

    const UnscaledCoordinates unscaled;
    if (backends->window) {
        const Status watched = watchWindow();
        if (watched != Status::Frame)
            return watched;
    }
    switch (path) {
    case Path::GraphicsCapture:
        return readGraphicsCapture(into);
    case Path::WindowByDuplication:
        return readWindowByDuplication(into);
    case Path::Duplication:
        break;
    }
    return readDuplication(into);
}

// Whether the window is still there and showing, and how large the picture of it is to be.
// Status::Frame when there is a window to read.
ScreenCapture::Status ScreenCapture::watchWindow()
{
    Backends &state = *backends;
    if (!IsWindow(state.window)) {
        qCInfo(LogVideo) << "The window being captured was closed";
        tearDown();
        closed = true;
        return Status::Closed;
    }

    // Graphics capture leaves out by itself a window that has asked to be kept out of
    // captures. Duplication would show it black, and it is not asked to draw itself either.
    DWORD affinity = 0;
    const bool keptOutOfCaptures = path == Path::WindowByDuplication && GetWindowDisplayAffinity(state.window, &affinity) && affinity != 0;
    const QRect client = IsIconic(state.window) || !IsWindowVisible(state.window) || keptOutOfCaptures ? QRect() : clientOnScreen(state.window);
    if (client.isEmpty()) {
        // Hiding a window ends its graphics capture, and minimising it does not.
        if (state.session && state.session->endedWith() != 0)
            state.session.reset();
        dropDuplication();
        state.clientSize = QSize();
        failingSince.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(STALLED_WAIT_MS));
        return Status::Hidden;
    }

    const auto now = Clock::now();
    if (client.size() != state.clientSize) {
        state.clientSize = client.size();
        state.sizeChangedAt = now;
    }
    const QSize wanted = evenSize(client.size()).expandedTo(SMALLEST_WINDOW_PICTURE);
    const bool held = now - state.sizeChangedAt >= std::chrono::milliseconds(RESIZE_SETTLE_MS);
    if (wanted != state.pictureSize && (state.pictureSize.isEmpty() || held)) {
        state.pictureSize = wanted;
        if (state.session)
            state.session->abandon();
        state.session.reset();
        av_buffer_unref(&state.pool);
    }
    return Status::Frame;
}

void ScreenCapture::delivered(AVFrame *picture, bool bt601)
{
    picturesFromGraph++;
    everDelivered = true;
    failingSince.reset();

    // The scaling filters hand on the description of the source's own RGB picture.
    picture->colorspace = bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    picture->color_range = AVCOL_RANGE_MPEG;
}

ScreenCapture::Status ScreenCapture::readDuplication(AVFrame *into)
{
    if (!graph) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        const int built = buildDuplication();
        if (built < 0)
            return afterFailure(built);
    }

    const int got = av_buffersink_get_frame(sink, into);
    if (got == AVERROR(EAGAIN))
        return Status::NoFrame;
    if (got < 0)
        return afterFailure(got);

    if (cardScaling == CardScaling::Suspected) {
        qCInfo(LogVideo) << "Scaling the screen on the graphics card failed; reading it back and scaling it in software";
        cardScaling = CardScaling::Untried;
        cardScalingRuledOut = true;
    } else if (graphScalesOnCard) {
        cardScaling = CardScaling::Works;
    }
    delivered(into, graphIsBt601);
    return Status::Frame;
}

ScreenCapture::Status ScreenCapture::readGraphicsCapture(AVFrame *into)
{
    Backends &state = *backends;
    if (!state.session) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        if (waitedForRetry(state.remakeAt))
            return Status::NoFrame;
        const int built = buildGraphicsCapture();
        if (built < 0 && path != Path::GraphicsCapture)
            return Status::Stalled;
        if (built < 0)
            return afterFailure(built);
    }

    const auto interval = pictureInterval(request.framerate);
    const auto giveUpAt = std::max(Clock::now(), state.pictureDue) + std::chrono::milliseconds(NOTHING_NEW_WAIT_MS);
    // A picture that comes a little before one is due is the one a source at twice the rate
    // has ready, and the next would be late.
    const GraphicsCaptureSession::Taken taken = state.session->take(state.captured.get(), state.pictureDue - interval / 4, giveUpAt);
    if (taken == GraphicsCaptureSession::Taken::Ended) {
        const int ended = state.session->endedWith();
        if (ended != AVERROR_EOF || !state.window)
            return afterFailure(ended);
        // Windows ends the capture of a window that is hidden or closed. Neither is a failure,
        // and which it was the next look at the window tells.
        state.session.reset();
        state.remakeAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        return Status::NoFrame;
    }
    if (taken == GraphicsCaptureSession::Taken::Nothing)
        return Status::NoFrame;
    state.pictureDue = std::max(state.pictureDue + interval, Clock::now() + interval / 2);

    AVFrame *captured = state.captured.get();
    const QSize ownSize(captured->width, captured->height);
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    if (request.keepOnCard && request.rgbOnCard && size == ownSize) {
        av_frame_move_ref(into, captured);
        delivered(into, false);
        return Status::Frame;
    }

    const bool bt601 = request.bt601Only || size.height() < HD_HEIGHT;
    int converted = prepareBgra(state.bgra.get(), ownSize) ? av_hwframe_transfer_data(state.bgra.get(), captured, 0) : AVERROR(ENOMEM);
    av_frame_unref(captured);
    if (converted >= 0)
        converted = state.converter.convert(state.bgra.get(), size, request.softwareFormat, bt601, into);
    if (converted < 0)
        return afterFailure(converted);
    delivered(into, bt601);
    return Status::Frame;
}

// The official client's way with a window on Windows 10. Every picture is first looked for on
// the screen, and where the screen cannot give the window by itself the window draws that
// picture. Either way it ends up the same size and in the same place, so the encoder never
// knows which it was.
ScreenCapture::Status ScreenCapture::readWindowByDuplication(AVFrame *into)
{
    Backends &state = *backends;
    std::this_thread::sleep_until(state.pictureDue);
    state.pictureDue = std::max(state.pictureDue + pictureInterval(request.framerate), Clock::now());

    const QRect client = clientOnScreen(state.window);
    const bool screenDuplicated = duplicateScreenOf(client);
    const QSize size = encodeSizeFor(state.pictureSize, request.maxHeight);
    const bool staysOnCard = state.pool && request.keepOnCard && request.rgbOnCard && size == state.pictureSize;
    const bool bt601 = !staysOnCard && (request.bt601Only || size.height() < HD_HEIGHT);

    AVFrame *card = state.captured.get();
    AVFrame *bgra = state.bgra.get();
    av_frame_unref(card);
    state.drawnBecause = "its screen cannot be duplicated";
    const bool cutOut = screenDuplicated && cutOutOfScreen(client, card);
    if (!cutOut && !drawnByItself(client))
        return Status::NoFrame;

    bool ready = true;
    if (cutOut && !staysOnCard)
        ready = prepareBgra(bgra, state.pictureSize) && av_hwframe_transfer_data(bgra, card, 0) >= 0;
    else if (!cutOut && staysOnCard)
        ready = av_hwframe_get_buffer(state.pool, card, 0) >= 0 && av_hwframe_transfer_data(card, bgra, 0) >= 0;
    if (ready && staysOnCard)
        av_frame_move_ref(into, card);
    else if (ready)
        ready = state.converter.convert(bgra, size, request.softwareFormat, bt601, into) >= 0;
    av_frame_unref(card);
    if (!ready)
        return Status::NoFrame;

    if (state.lastWasCutOut != cutOut) {
        state.lastWasCutOut = cutOut;
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << (staysOnCard ? "on the graphics card," : "scaled in software,") << (cutOut ? "cut out of its screen" : "drawn by itself, as") << (cutOut ? "" : state.drawnBecause);
    }
    (cutOut ? state.cutOutPictures : state.drawnPictures)++;
    delivered(into, bt601);
    return Status::Frame;
}

// Sees to it that the screen under the middle of the window is being duplicated, and that
// there is a card to keep the window's pictures on. False when that screen cannot be read now.
bool ScreenCapture::duplicateScreenOf(const QRect &client)
{
    Backends &state = *backends;
    const QPoint middle = client.center();
    const HMONITOR under = MonitorFromPoint({ middle.x(), middle.y() }, MONITOR_DEFAULTTONULL);
    const bool retryDue = Clock::now() >= retryAt;
    if (under != state.screenHandle || (state.screenChanged && retryDue)) {
        dropDuplication();
        state.screenHandle = under;
        state.screenChanged = false;
        state.screen.reset();
        for (const Monitor &each : attachedMonitors()) {
            if (each.handle == under)
                state.screen = each;
        }
    }

    const int adapter = state.screen ? state.screen->adapter : std::max(0, state.deviceAdapter);
    if (adapter != state.deviceAdapter || (!state.device && retryDue)) {
        dropDuplication();
        av_buffer_unref(&state.pool);
        av_buffer_unref(&state.device);
        state.deviceAdapter = adapter;
        if (av_hwdevice_ctx_create(&state.device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(adapter).constData(), nullptr, 0) < 0)
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    }
    if (state.device && !state.pool)
        state.pool = cardPictures(state.device, state.pictureSize);

    // ddagrab does not turn a turned screen's picture upright.
    if (!state.pool || !state.screen || state.screen->turned)
        return false;
    if (graph)
        return true;
    if (!retryDue)
        return false;

    const QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(state.screen->output) + ":framerate=" + QByteArray::number(request.framerate);
    const int built = assemble(chain, state.device, &graph, &sink);
    if (built < 0) {
        // Until Windows gives the screen back, which it also takes for a mode it may come
        // back in another size with.
        state.screenChanged = true;
        retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    }
    return built >= 0;
}

// The window's picture out of the screen's, onto `card`. False when the screen does not show
// the window by itself, or not where it was when the screen was read.
bool ScreenCapture::cutOutOfScreen(const QRect &client, AVFrame *card)
{
    Backends &state = *backends;
    // While its size is changing, and where it is smaller than what is sent, the window does
    // not fill the picture.
    state.drawnBecause = "its size is changing, or is smaller than what is sent";
    if (evenSize(client.size()) != state.pictureSize)
        return false;
    state.drawnBecause = "a window above overlaps it";
    if (isCoveredOnScreen(state.window, client))
        return false;

    state.drawnBecause = "it reaches past its screen";
    const QRect screen = state.screen->geometry;
    if (!screen.adjusted(-REACH_PAST_SCREEN, -REACH_PAST_SCREEN, REACH_PAST_SCREEN, REACH_PAST_SCREEN).contains(client))
        return false;
    const QRect onScreen = client & screen;
    const QRect inPicture = QRect(onScreen.topLeft() - client.topLeft(), onScreen.size()) & QRect(QPoint(0, 0), state.pictureSize);

    state.drawnBecause = "its screen had no picture";
    AVFrame *whole = state.wholeScreen.get();
    av_frame_unref(whole);
    const int got = av_buffersink_get_frame(sink, whole);
    if (got < 0) {
        if (got != AVERROR(EAGAIN)) {
            dropDuplication();
            state.screenChanged = true;
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        }
        return false;
    }

    // The read waits for the screen, and a window moved or covered meanwhile has left its
    // place there to something else.
    state.drawnBecause = "it moved or was covered while its screen was read";
    const bool cut = clientOnScreen(state.window) == client && !isCoveredOnScreen(state.window, client) && av_hwframe_get_buffer(state.pool, card, 0) >= 0;
    if (cut) {
        if (inPicture.size() != state.pictureSize)
            clearToBlack(card);
        copyOnCard(whole, inPicture.translated(client.topLeft() - screen.topLeft()), card, inPicture.topLeft());
    }
    av_frame_unref(whole);
    return cut;
}

// The window's picture as the window draws it, into the BGRA picture kept for it. False when
// it would not draw.
bool ScreenCapture::drawnByItself(const QRect &client)
{
    Backends &state = *backends;
    if (!state.drawer)
        state.drawer = WindowDrawer::start(state.window, true);
    if (!state.drawer->draw(std::chrono::milliseconds(DRAWING_LIMIT_MS)))
        return false;
    state.drawer->drawPointer(client);

    const Bitmap &drawn = state.drawer->drawn();
    AVFrame *bgra = state.bgra.get();
    if (drawn.size() == client.size()) {
        if (!prepareBgra(bgra, state.pictureSize))
            return false;
        copyTopLeft(drawn, bgra);
        return true;
    }

    // A program Windows scales for the screen draws itself at its own, smaller size.
    AVFrame *asDrawn = state.asDrawn.get();
    if (!prepareBgra(asDrawn, drawn.size()))
        return false;
    copyTopLeft(drawn, asDrawn);
    av_frame_unref(bgra);
    return state.rescaler.convert(asDrawn, state.pictureSize, AV_PIX_FMT_BGRA, false, bgra) >= 0;
}

// Neither source recovers from losing what it captures: ddagrab not from losing the screen nor
// from a changed screen mode, graphics capture not from its window being hidden. The capture
// has to be made again, and until Windows gives the source back that fails.
ScreenCapture::Status ScreenCapture::afterFailure(int error)
{
    const bool scalingSuspected = graphScalesOnCard && picturesFromGraph == 0 && cardScaling == CardScaling::Untried;
    tearDown();
    if (scalingSuspected) {
        cardScaling = CardScaling::Suspected;
        return Status::Stalled;
    }
    // The capture without it failed as well, so the fault lies elsewhere.
    if (cardScaling == CardScaling::Suspected)
        cardScaling = CardScaling::Untried;

    const auto now = Clock::now();
    // Access is denied for as long as the secure desktop shows, which is as long as the
    // user leaves a UAC prompt open or the machine locked, and no reason to stop sharing.
    const bool secureDesktopShowing = error == AVERROR(EPERM) && everDelivered;
    if (!failingSince || secureDesktopShowing)
        failingSince = now;
    if (now - *failingSince >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) {
        failed = true;
        const QString reason = backends->window ? QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "The window can no longer be captured (%1).") : QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "The screen can no longer be captured (%1).");
        failureText = reason.arg(describeError(error));
        qCWarning(LogVideo) << "Giving up on capturing" << request.sourceId << ":" << describeError(error);
        return Status::Failed;
    }
    retryAt = now + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    return Status::Stalled;
}

int ScreenCapture::buildDuplication()
{
    graphScalesOnCard = false;
    picturesFromGraph = 0;
    const std::vector<Monitor> monitors = attachedMonitors();
    const Monitor *monitor = numberedMonitor(monitors, request.sourceId);
    if (!monitor)
        return AVERROR(ENODEV);

    // ddagrab counts outputs on the adapter of the device it is given.
    AVBufferRef *device = nullptr;
    int result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(monitor->adapter).constData(), nullptr, 0);
    if (result < 0)
        return result;

    const QSize ownSize = monitor->onItsSide ? monitor->geometry.size().transposed() : monitor->geometry.size();
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const bool asCaptured = request.keepOnCard && request.rgbOnCard && size == ownSize;
    graphScalesOnCard = !asCaptured && !cardScalingRuledOut && cardScaling != CardScaling::Suspected && avfilter_get_by_name("scale_d3d11");
    // The card's video processor converts to BT.601 unless told otherwise, and scale_d3d11
    // does not tell it. An encoder converting for itself is asked for BT.709 at any size:
    // AMF makes something else of a request for BT.601.
    graphIsBt601 = !asCaptured && (graphScalesOnCard || request.bt601Only || size.height() < HD_HEIGHT);

    QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(monitor->output) + ":framerate=" + QByteArray::number(request.framerate);
    if (asCaptured) {
        // Nothing to scale, and the encoder converts.
    } else if (graphScalesOnCard) {
        chain += ",scale_d3d11=width=" + QByteArray::number(size.width()) + ":height=" + QByteArray::number(size.height()) + ":format=nv12";
        if (!request.keepOnCard)
            chain += ",hwdownload,format=nv12,format=" + QByteArray(av_get_pix_fmt_name(static_cast<AVPixelFormat>(request.softwareFormat)));
    } else {
        chain += ",hwdownload,format=bgra," + softwareScaling(size, graphIsBt601, request.softwareFormat);
    }

    result = assemble(chain, device, &graph, &sink);
    av_buffer_unref(&device);

    if (result >= 0)
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << (asCaptured ? "kept on the graphics card as it is" : !graphScalesOnCard ? "read back and scaled in software"
                                                                                                                                                            : request.keepOnCard       ? "scaled and kept on the graphics card"
                                                                                                                                                                                       : "scaled on the graphics card and read back");
    return result;
}

int ScreenCapture::buildGraphicsCapture()
{
    Backends &state = *backends;
    picturesFromGraph = 0;

    QSize ownSize = state.pictureSize;
    QByteArray source;
    QByteArray adapter;
    if (state.window) {
        source = ":hwnd=" + QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(state.window)));
    } else {
        const std::vector<Monitor> monitors = attachedMonitors();
        const Monitor *monitor = numberedMonitor(monitors, request.sourceId);
        if (!monitor)
            return AVERROR(ENODEV);
        ownSize = evenSize(monitor->geometry.size());
        source = ":hmonitor=" + QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(monitor->handle)));
        adapter = QByteArray::number(monitor->adapter);
    }

    // The size is given because the filter otherwise fixes it before the first picture, from
    // what Windows says the window measures, which is not always what it then hands over.
    const QByteArray chain = "gfxcapture=max_framerate=" + QByteArray::number(request.framerate) + source + ":width=" + QByteArray::number(ownSize.width()) + ":height=" + QByteArray::number(ownSize.height());
    int result = 0;
    state.session = GraphicsCaptureSession::start(chain, adapter, &result);
    if (result >= 0) {
        const QSize size = encodeSizeFor(ownSize, request.maxHeight);
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "by graphics capture as" << size.width() << "x" << size.height() << (request.keepOnCard && request.rgbOnCard && size == ownSize ? "kept on the graphics card as it is" : "read back and scaled in software");
        return result;
    }

    if (result == AVERROR(ENOSYS))
        graphicsCaptureRuledOut = true;
    // Left to choose, what graphics capture will not take is duplicated.
    const bool leftToChoose = request.method == CaptureMethod::Auto && state.window && !everDelivered;
    if ((result == AVERROR(ENOSYS) || leftToChoose) && screenCaptureAvailable()) {
        qCInfo(LogVideo) << "Graphics capture of" << request.sourceId << "failed (" << describeError(result) << "); using desktop duplication";
        path = state.window ? Path::WindowByDuplication : Path::Duplication;
    }
    return result;
}

void ScreenCapture::dropDuplication()
{
    avfilter_graph_free(&graph);
    sink = nullptr;
}

void ScreenCapture::tearDown()
{
    dropDuplication();
    if (backends->session)
        backends->session->abandon();
    backends->session.reset();
    if (backends->drawer)
        backends->drawer->abandon();
    backends->drawer.reset();
}

#else // ACHERON_SCREEN_CAPTURE

struct ScreenCapture::Backends
{
};

bool screenCaptureAvailable()
{
    return false;
}

bool windowCaptureAvailable()
{
    return false;
}

bool captureMethodUsable(CaptureMethod)
{
    return false;
}

QList<CaptureSource> captureSources()
{
    return {};
}

QList<CaptureSource> captureWindows()
{
    return {};
}

QImage captureThumbnail(const CaptureSource &, const QSize &)
{
    return {};
}

void restoreIfMinimised(const CaptureSource &)
{
}

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::configure(const Request &wanted)
{
    request = wanted;
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return false;
}

QString ScreenCapture::captureName() const
{
    return {};
}

ScreenCapture::Status ScreenCapture::read(AVFrame *)
{
    failureText = QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "This build cannot capture the screen.");
    return Status::Failed;
}

#endif // ACHERON_SCREEN_CAPTURE

} // namespace Video
} // namespace Core
} // namespace Acheron
