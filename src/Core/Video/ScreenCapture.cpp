#include "Core/Video/ScreenCapture.hpp"

#include "Core/Logging.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QOperatingSystemVersion>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

// Capture is ffmpeg's desktop duplication source, which is in libavfilter and Windows only.
#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_SCREEN_CAPTURE
#endif

#ifdef ACHERON_SCREEN_CAPTURE
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <windows.h>

#include <dxgi.h>
#endif

namespace Acheron {
namespace Core {
namespace Video {

QSize encodeSizeFor(const QSize &source, int maxHeight)
{
    QSize size = source;
    const QSize bound(maxHeight * 16 / 9, maxHeight);
    if (maxHeight > 0 && (source.width() > bound.width() || source.height() > bound.height()))
        size = source.scaled(bound, Qt::KeepAspectRatio);
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

#ifdef ACHERON_SCREEN_CAPTURE

namespace {

const QLatin1String SOURCE_ID_PREFIX("screen:");
constexpr int HD_HEIGHT = 720;
constexpr int STALLED_WAIT_MS = 50;

// Whether scale_d3d11 was found not to work on this machine. It asks the card for an array of
// NV12 render targets, which a Radeon RX 7800 XT for one refuses to make.
std::atomic<bool> cardScalingRuledOut{ false };

// Windows scales the coordinates it gives a thread that has not said it understands display
// scaling, and a thread of a program without a window has not.
class UnscaledCoordinates
{
public:
    UnscaledCoordinates()
    {
        if (setAwareness)
            previous = setAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~UnscaledCoordinates()
    {
        if (previous)
            setAwareness(previous);
    }

private:
    // Not there before Windows 10 1607.
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    static inline const SetAwareness setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    DPI_AWARENESS_CONTEXT previous = nullptr;
};

struct Monitor
{
    int adapter = 0;
    // Its place among the adapter's outputs, attached to the desktop or not, which is how
    // ddagrab counts them.
    int output = 0;
    QRect geometry;
    // Turned a quarter, in which case Windows hands its picture over lying on its side.
    bool onItsSide = false;
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
                monitor.geometry = QRect(mode.dmPosition.x, mode.dmPosition.y, static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight));
                monitor.onItsSide = description.Rotation == DXGI_MODE_ROTATION_ROTATE90 || description.Rotation == DXGI_MODE_ROTATION_ROTATE270;
                found.push_back(monitor);
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

QString describeError(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return QString::fromUtf8(text);
}

} // namespace

bool screenCaptureAvailable()
{
    // Desktop duplication came with Windows 8.
    static const bool available = avfilter_get_by_name("ddagrab") && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8;
    return available;
}

QList<CaptureSource> captureSources()
{
    QList<CaptureSource> sources;
    if (!screenCaptureAvailable())
        return sources;

    const std::vector<Monitor> monitors = attachedMonitors();
    for (size_t i = 0; i < monitors.size(); i++) {
        CaptureSource source;
        source.id = SOURCE_ID_PREFIX + QString::number(i);
        source.name = QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "Screen %1").arg(i + 1);
        source.geometry = monitors[i].geometry;
        sources.append(source);
    }
    return sources;
}

QImage captureThumbnail(const CaptureSource &source, const QSize &bound)
{
    const QRect area = source.geometry;
    if (area.isEmpty() || bound.isEmpty())
        return {};
    const QSize size = area.width() > bound.width() || area.height() > bound.height() ? area.size().scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)) : area.size();

    const UnscaledCoordinates unscaled;
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO layout = {};
    layout.bmiHeader.biSize = sizeof(layout.bmiHeader);
    layout.bmiHeader.biWidth = size.width();
    // Negative for rows that run from the top down.
    layout.bmiHeader.biHeight = -size.height();
    layout.bmiHeader.biPlanes = 1;
    layout.bmiHeader.biBitCount = 32;
    layout.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(memory, &layout, DIB_RGB_COLORS, &pixels, nullptr, 0);

    QImage thumbnail;
    if (bitmap) {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
        if (StretchBlt(memory, 0, 0, size.width(), size.height(), screen, area.x(), area.y(), area.width(), area.height(), SRCCOPY)) {
            GdiFlush();
            thumbnail = QImage(size, QImage::Format_RGB32);
            const auto *from = static_cast<const quint32 *>(pixels);
            for (int y = 0; y < size.height(); y++) {
                auto *to = reinterpret_cast<quint32 *>(thumbnail.scanLine(y));
                // GDI leaves the byte an image keeps for opacity at zero.
                for (int x = 0; x < size.width(); x++)
                    to[x] = from[y * size.width() + x] | 0xFF000000u;
            }
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return thumbnail;
}

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
    failed = false;
    failureText.clear();
}

ScreenCapture::Status ScreenCapture::read(AVFrame *into)
{
    if (failed)
        return Status::Failed;

    if (!graph) {
        const auto now = std::chrono::steady_clock::now();
        if (now < retryAt) {
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(retryAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)));
            return Status::Stalled;
        }
        const int built = build();
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
    picturesFromGraph++;
    everDelivered = true;
    failingSince.reset();

    // The scaling filters hand on the description of the screen's own RGB picture.
    into->colorspace = graphIsBt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    into->color_range = AVCOL_RANGE_MPEG;
    return Status::Frame;
}

// ddagrab does not recover from losing the screen, nor from a changed screen mode: the capture
// has to be made again, and until Windows gives the screen back that fails.
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

    const auto now = std::chrono::steady_clock::now();
    // Access is denied for as long as the secure desktop shows, which is as long as the
    // user leaves a UAC prompt open or the machine locked, and no reason to stop sharing.
    const bool secureDesktopShowing = error == AVERROR(EPERM) && everDelivered;
    if (!failingSince || secureDesktopShowing)
        failingSince = now;
    if (now - *failingSince >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) {
        failed = true;
        failureText = QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "The screen can no longer be captured (%1).").arg(describeError(error));
        qCWarning(LogVideo) << "Giving up on capturing" << request.sourceId << ":" << describeError(error);
        return Status::Failed;
    }
    retryAt = now + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    return Status::Stalled;
}

int ScreenCapture::build()
{
    graphScalesOnCard = false;
    picturesFromGraph = 0;
    const std::vector<Monitor> monitors = attachedMonitors();
    bool numbered = false;
    const int number = request.sourceId.mid(SOURCE_ID_PREFIX.size()).toInt(&numbered);
    if (!request.sourceId.startsWith(SOURCE_ID_PREFIX) || !numbered || number < 0 || number >= static_cast<int>(monitors.size()))
        return AVERROR(ENODEV);
    const Monitor &monitor = monitors[static_cast<size_t>(number)];

    // ddagrab counts outputs on the adapter of the device it is given.
    AVBufferRef *device = nullptr;
    int result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(monitor.adapter).constData(), nullptr, 0);
    if (result < 0)
        return result;

    const QSize ownSize = monitor.onItsSide ? monitor.geometry.size().transposed() : monitor.geometry.size();
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const QByteArray softwareFormat = av_get_pix_fmt_name(static_cast<AVPixelFormat>(request.softwareFormat));
    const bool asCaptured = request.keepOnCard && request.rgbOnCard && size == ownSize;
    graphScalesOnCard = !asCaptured && !cardScalingRuledOut && cardScaling != CardScaling::Suspected && avfilter_get_by_name("scale_d3d11");
    // The card's video processor converts to BT.601 unless told otherwise, and scale_d3d11
    // does not tell it. An encoder converting for itself is asked for BT.709 at any size:
    // AMF makes something else of a request for BT.601.
    graphIsBt601 = !asCaptured && (graphScalesOnCard || request.bt601Only || size.height() < HD_HEIGHT);

    QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(monitor.output) + ":framerate=" + QByteArray::number(request.framerate);
    if (asCaptured) {
        // Nothing to scale, and the encoder converts.
    } else if (graphScalesOnCard) {
        chain += ",scale_d3d11=width=" + QByteArray::number(size.width()) + ":height=" + QByteArray::number(size.height()) + ":format=nv12";
        if (!request.keepOnCard)
            chain += ",hwdownload,format=nv12,format=" + softwareFormat;
    } else {
        chain += ",hwdownload,format=bgra,scale=w=" + QByteArray::number(size.width()) + ":h=" + QByteArray::number(size.height()) + ":flags=bilinear:out_range=tv:out_color_matrix=" + (graphIsBt601 ? "bt601" : "bt709") + ",format=" + softwareFormat;
    }

    graph = avfilter_graph_alloc();
    result = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "sink", nullptr, nullptr, graph);
    if (result >= 0) {
        AVFilterInOut *inputs = nullptr;
        AVFilterInOut *outputs = nullptr;
        result = avfilter_graph_parse2(graph, chain.constData(), &inputs, &outputs);
        if (result >= 0)
            result = outputs && !outputs->next && !inputs ? avfilter_link(outputs->filter_ctx, static_cast<unsigned>(outputs->pad_idx), sink, 0) : AVERROR(EINVAL);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    }
    for (unsigned i = 0; result >= 0 && i < graph->nb_filters; i++) {
        if (graph->filters[i]->filter->flags & AVFILTER_FLAG_HWDEVICE)
            graph->filters[i]->hw_device_ctx = av_buffer_ref(device);
    }
    if (result >= 0)
        result = avfilter_graph_config(graph, nullptr);
    av_buffer_unref(&device);

    if (result >= 0)
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << (asCaptured ? "kept on the graphics card as it is" : !graphScalesOnCard ? "read back and scaled in software"
                                                                                                                                                            : request.keepOnCard       ? "scaled and kept on the graphics card"
                                                                                                                                                                                       : "scaled on the graphics card and read back");
    return result;
}

void ScreenCapture::tearDown()
{
    avfilter_graph_free(&graph);
    sink = nullptr;
}

#else // ACHERON_SCREEN_CAPTURE

bool screenCaptureAvailable()
{
    return false;
}

QList<CaptureSource> captureSources()
{
    return {};
}

QImage captureThumbnail(const CaptureSource &, const QSize &)
{
    return {};
}

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::configure(const Request &wanted)
{
    request = wanted;
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
