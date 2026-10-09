#include "D3D11GlBridge.hpp"

#ifdef Q_OS_WIN

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <cstring>

#include <d3d11_4.h>
#include <dxgi1_4.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if __has_include(<libavutil/hwcontext_d3d11va.h>)
#include <libavutil/hwcontext_d3d11va.h>
#define ACHERON_HAVE_D3D11VA 1
#endif
}

namespace Acheron {
namespace UI {

namespace {

constexpr GLenum ACCESS_READ_ONLY = 0x0000;
constexpr DWORD DRAW_TIMEOUT_MS = 100;
constexpr DXGI_FORMAT TARGET_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;

template <typename T>
void drop(T *&object)
{
    if (object)
        object->Release();
    object = nullptr;
}

} // namespace

// WGL_NV_DX_interop2, looked up in the context it is used from.
struct D3D11GlBridge::Wgl
{
    HANDLE(WINAPI *openDevice)(void *) = nullptr;
    BOOL(WINAPI *closeDevice)(HANDLE) = nullptr;
    HANDLE(WINAPI *registerObject)(HANDLE, void *, GLuint, GLenum, GLenum) = nullptr;
    BOOL(WINAPI *unregisterObject)(HANDLE, HANDLE) = nullptr;
    BOOL(WINAPI *lockObjects)(HANDLE, GLint, HANDLE *) = nullptr;
    BOOL(WINAPI *unlockObjects)(HANDLE, GLint, HANDLE *) = nullptr;

    static std::unique_ptr<Wgl> resolve()
    {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        // ANGLE is OpenGL ES over Direct3D and has no WGL.
        if (!context || context->isOpenGLES())
            return nullptr;

        const auto extensions = reinterpret_cast<const char *(WINAPI *)()>(context->getProcAddress("wglGetExtensionsStringEXT"));
        const char *list = extensions ? extensions() : nullptr;
        if (!list || !std::strstr(list, "WGL_NV_DX_interop2"))
            return nullptr;

        auto wgl = std::make_unique<Wgl>();
        wgl->openDevice = reinterpret_cast<decltype(wgl->openDevice)>(context->getProcAddress("wglDXOpenDeviceNV"));
        wgl->closeDevice = reinterpret_cast<decltype(wgl->closeDevice)>(context->getProcAddress("wglDXCloseDeviceNV"));
        wgl->registerObject = reinterpret_cast<decltype(wgl->registerObject)>(context->getProcAddress("wglDXRegisterObjectNV"));
        wgl->unregisterObject = reinterpret_cast<decltype(wgl->unregisterObject)>(context->getProcAddress("wglDXUnregisterObjectNV"));
        wgl->lockObjects = reinterpret_cast<decltype(wgl->lockObjects)>(context->getProcAddress("wglDXLockObjectsNV"));
        wgl->unlockObjects = reinterpret_cast<decltype(wgl->unlockObjects)>(context->getProcAddress("wglDXUnlockObjectsNV"));
        if (!wgl->openDevice || !wgl->closeDevice || !wgl->registerObject || !wgl->unregisterObject || !wgl->lockObjects || !wgl->unlockObjects)
            return nullptr;
        return wgl;
    }
};

D3D11GlBridge::D3D11GlBridge() = default;

D3D11GlBridge::~D3D11GlBridge()
{
    // release() has run by now wherever there was an OpenGL context to run it in.
    av_buffer_unref(&hardwareDevice);
}

#ifndef ACHERON_HAVE_D3D11VA

bool D3D11GlBridge::contextSupportsIt()
{
    return false;
}

bool D3D11GlBridge::update(const AVFrame *, QSize, bool, bool)
{
    return false;
}

bool D3D11GlBridge::lock()
{
    return false;
}

void D3D11GlBridge::unlock() { }

void D3D11GlBridge::release() { }

#else

namespace {

AVD3D11VADeviceContext *direct3d(AVBufferRef *device)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
}

// The device's one drawing context is shared with every decoder using the device.
class DeviceLock
{
public:
    explicit DeviceLock(AVD3D11VADeviceContext *device)
        : device(device)
    {
        device->lock(device->lock_ctx);
    }
    ~DeviceLock() { device->unlock(device->lock_ctx); }
    DeviceLock(const DeviceLock &) = delete;
    DeviceLock &operator=(const DeviceLock &) = delete;

private:
    AVD3D11VADeviceContext *device;
};

void setColourSpaces(ID3D11VideoContext *context, ID3D11VideoProcessor *processor, bool bt709, bool fullRange)
{
    // The older call leaves the meaning of the RGB range to the driver, and drivers disagree.
    ID3D11VideoContext1 *modern = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&modern)))) {
        DXGI_COLOR_SPACE_TYPE input;
        if (bt709)
            input = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        else
            input = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        modern->VideoProcessorSetStreamColorSpace1(processor, 0, input);
        modern->VideoProcessorSetOutputColorSpace1(processor, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        modern->Release();
        return;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input = {};
    input.YCbCr_Matrix = bt709 ? 1 : 0;
    input.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    context->VideoProcessorSetStreamColorSpace(processor, 0, &input);
    // All zero is full-range RGB.
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE output = {};
    context->VideoProcessorSetOutputColorSpace(processor, &output);
}

} // namespace

bool D3D11GlBridge::contextSupportsIt()
{
    return Wgl::resolve() != nullptr;
}

bool D3D11GlBridge::openDevice(const AVFrame *frame)
{
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
    if (hardwareDevice && hardwareDevice->data == frames->device_ref->data)
        return true;

    release();
    if (!wgl)
        wgl = Wgl::resolve();
    if (!wgl)
        return false;

    const AVD3D11VADeviceContext *device = static_cast<const AVD3D11VADeviceContext *>(frames->device_ctx->hwctx);
    if (!device->device || !device->video_device || !device->video_context || !device->lock || !device->unlock)
        return false;

    interopDevice = wgl->openDevice(device->device);
    if (!interopDevice)
        return false;
    hardwareDevice = av_buffer_ref(frames->device_ref);
    if (!hardwareDevice)
        return false;

    ID3D11Device5 *modern = nullptr;
    if (SUCCEEDED(device->device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&modern)))) {
        if (SUCCEEDED(modern->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&fence))))
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        modern->Release();
    }
    fenceValue = 0;
    return true;
}

bool D3D11GlBridge::createTarget(int inputFormat, QSize input, QSize output)
{
    releaseTarget();
    AVD3D11VADeviceContext *device = direct3d(hardwareDevice);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = { 60, 1 };
    content.InputWidth = static_cast<UINT>(input.width());
    content.InputHeight = static_cast<UINT>(input.height());
    content.OutputFrameRate = { 60, 1 };
    content.OutputWidth = static_cast<UINT>(output.width());
    content.OutputHeight = static_cast<UINT>(output.height());
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(device->video_device->CreateVideoProcessorEnumerator(&content, &enumerator)))
        return false;

    UINT support = 0;
    if (FAILED(enumerator->CheckVideoProcessorFormat(static_cast<DXGI_FORMAT>(inputFormat), &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
        return false;
    if (FAILED(enumerator->CheckVideoProcessorFormat(TARGET_FORMAT, &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        return false;
    if (FAILED(device->video_device->CreateVideoProcessor(enumerator, 0, &processor)))
        return false;

    D3D11_TEXTURE2D_DESC description = {};
    description.Width = static_cast<UINT>(output.width());
    description.Height = static_cast<UINT>(output.height());
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = TARGET_FORMAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->device->CreateTexture2D(&description, nullptr, &target)))
        return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view = {};
    view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(device->video_device->CreateVideoProcessorOutputView(target, enumerator, &view, &outputView)))
        return false;

    QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
    gl->glGenTextures(1, &glTexture);
    interopObject = wgl->registerObject(interopDevice, target, glTexture, GL_TEXTURE_2D, ACCESS_READ_ONLY);
    if (!interopObject || !lock())
        return false;
    gl->glBindTexture(GL_TEXTURE_2D, glTexture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    unlock();

    {
        DeviceLock guard(device);
        ID3D11VideoContext *context = device->video_context;
        context->VideoProcessorSetStreamFrameFormat(processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        // No sharpening, noise reduction or the like: the picture is shown as it was sent.
        context->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
        // The texture is as large as the coded picture, which is rounded up to whole blocks.
        const RECT source = { 0, 0, input.width(), input.height() };
        context->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &source);
        const RECT whole = { 0, 0, output.width(), output.height() };
        context->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &whole);
        context->VideoProcessorSetOutputTargetRect(processor, TRUE, &whole);
    }

    targetInputFormat = inputFormat;
    inputSize = input;
    outputSize = output;
    return true;
}

ID3D11VideoProcessorInputView *D3D11GlBridge::inputView(ID3D11Texture2D *texture, unsigned int slice)
{
    const auto key = std::make_pair(texture, slice);
    const auto found = inputViews.find(key);
    if (found != inputViews.end())
        return found->second;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description = {};
    description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    description.Texture2D.ArraySlice = slice;
    ID3D11VideoProcessorInputView *view = nullptr;
    if (FAILED(direct3d(hardwareDevice)->video_device->CreateVideoProcessorInputView(texture, enumerator, &description, &view)))
        return nullptr;
    inputViews.emplace(key, view);
    return view;
}

bool D3D11GlBridge::update(const AVFrame *frame, QSize size, bool bt709, bool fullRange)
{
    if (frame->format != AV_PIX_FMT_D3D11 || !frame->hw_frames_ctx || !frame->data[0] || size.isEmpty())
        return false;
    if (!openDevice(frame))
        return false;

    auto *texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
    const auto slice = static_cast<unsigned int>(reinterpret_cast<quintptr>(frame->data[1]));
    D3D11_TEXTURE2D_DESC description;
    texture->GetDesc(&description);

    const QSize input(frame->width, frame->height);
    if (!target || input != inputSize || size != outputSize || static_cast<int>(description.Format) != targetInputFormat) {
        if (!createTarget(static_cast<int>(description.Format), input, size)) {
            releaseTarget();
            return false;
        }
    }

    // A reopened decoder has new textures; views of the old ones would only keep them alive.
    if (frame->hw_frames_ctx->data != framePool) {
        for (auto &[key, view] : inputViews)
            view->Release();
        inputViews.clear();
        framePool = frame->hw_frames_ctx->data;
    }
    ID3D11VideoProcessorInputView *view = inputView(texture, slice);
    if (!view)
        return false;

    // OpenGL has to let go of the target while Direct3D draws into it.
    unlock();

    AVD3D11VADeviceContext *device = direct3d(hardwareDevice);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = view;

    bool signalled = false;
    {
        DeviceLock guard(device);
        setColourSpaces(device->video_context, processor, bt709, fullRange);
        if (FAILED(device->video_context->VideoProcessorBlt(processor, outputView, 0, 1, &stream)))
            return false;

        ID3D11DeviceContext4 *modern = nullptr;
        if (fence && fenceEvent && SUCCEEDED(device->device_context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&modern)))) {
            signalled = SUCCEEDED(modern->Signal(fence, ++fenceValue));
            modern->Release();
            device->device_context->Flush();
        }
    }

    // OpenGL's lock on the target waits for this drawing as well, but it spins while it does.
    // Waiting here first costs no processor time, and leaves the lock nothing to wait for.
    if (signalled && SUCCEEDED(fence->SetEventOnCompletion(fenceValue, fenceEvent)))
        WaitForSingleObject(fenceEvent, DRAW_TIMEOUT_MS);
    return true;
}

bool D3D11GlBridge::lock()
{
    if (locked)
        return true;
    if (!interopObject)
        return false;
    HANDLE object = interopObject;
    locked = wgl->lockObjects(interopDevice, 1, &object) != FALSE;
    return locked;
}

void D3D11GlBridge::unlock()
{
    if (!locked)
        return;
    HANDLE object = interopObject;
    wgl->unlockObjects(interopDevice, 1, &object);
    locked = false;
}

void D3D11GlBridge::releaseTarget()
{
    unlock();
    if (interopObject)
        wgl->unregisterObject(interopDevice, interopObject);
    interopObject = nullptr;
    if (glTexture) {
        if (QOpenGLContext *context = QOpenGLContext::currentContext())
            context->functions()->glDeleteTextures(1, &glTexture);
    }
    glTexture = 0;

    for (auto &[key, view] : inputViews)
        view->Release();
    inputViews.clear();
    framePool = nullptr;
    drop(outputView);
    drop(processor);
    drop(enumerator);
    drop(target);
    inputSize = {};
    outputSize = {};
}

void D3D11GlBridge::release()
{
    releaseTarget();
    if (interopDevice)
        wgl->closeDevice(interopDevice);
    interopDevice = nullptr;
    drop(fence);
    if (fenceEvent)
        CloseHandle(fenceEvent);
    fenceEvent = nullptr;
    av_buffer_unref(&hardwareDevice);
}

#endif // ACHERON_HAVE_D3D11VA

} // namespace UI
} // namespace Acheron

#endif // Q_OS_WIN
