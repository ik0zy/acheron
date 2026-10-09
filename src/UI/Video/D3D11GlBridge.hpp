#pragma once

#include <QSize>
#include <QtGlobal>

#ifdef Q_OS_WIN

#include <map>
#include <memory>
#include <utility>

struct AVBufferRef;
struct AVFrame;
struct ID3D11Fence;
struct ID3D11Texture2D;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorInputView;
struct ID3D11VideoProcessorOutputView;

namespace Acheron {
namespace UI {

// Shows a picture decoded by Direct3D 11 in an OpenGL context without it ever leaving the
// graphics card. The decoder's texture is converted to RGB and scaled by the card's video
// processor into a texture that OpenGL is allowed to read (WGL_NV_DX_interop2), so nothing is
// copied back to main memory and nothing is uploaded.
//
// Every method needs the OpenGL context current. One bridge serves one surface.
class D3D11GlBridge
{
public:
    // Whether the current OpenGL context can read Direct3D textures at all.
    [[nodiscard]] static bool contextSupportsIt();

    D3D11GlBridge();
    ~D3D11GlBridge();
    D3D11GlBridge(const D3D11GlBridge &) = delete;
    D3D11GlBridge &operator=(const D3D11GlBridge &) = delete;

    // Renders the frame, which must be a Direct3D 11 one, at `size` pixels. False when this
    // machine cannot do it, in which case the caller should stop taking such frames.
    bool update(const AVFrame *frame, QSize size, bool bt709, bool fullRange);

    // The OpenGL texture holding the picture. It may only be drawn between lock() and unlock().
    [[nodiscard]] unsigned int texture() const { return glTexture; }
    bool lock();
    void unlock();

    // Lets go of everything. The destructor cannot, having no OpenGL context to do it in.
    void release();

private:
    struct Wgl;

    bool openDevice(const AVFrame *frame);
    bool createTarget(int inputFormat, QSize input, QSize output);
    ID3D11VideoProcessorInputView *inputView(ID3D11Texture2D *texture, unsigned int slice);
    void releaseTarget();

    std::unique_ptr<Wgl> wgl;
    AVBufferRef *hardwareDevice = nullptr;
    void *interopDevice = nullptr;
    // For waiting until Direct3D has drawn, where the system has such a thing (Windows 10).
    ID3D11Fence *fence = nullptr;
    void *fenceEvent = nullptr;
    quint64 fenceValue = 0;

    ID3D11Texture2D *target = nullptr;
    ID3D11VideoProcessorEnumerator *enumerator = nullptr;
    ID3D11VideoProcessor *processor = nullptr;
    ID3D11VideoProcessorOutputView *outputView = nullptr;
    // The decoder goes round a fixed set of textures; a view of each is kept.
    std::map<std::pair<ID3D11Texture2D *, unsigned int>, ID3D11VideoProcessorInputView *> inputViews;
    const void *framePool = nullptr;
    void *interopObject = nullptr;
    unsigned int glTexture = 0;
    int targetInputFormat = 0;
    QSize inputSize;
    QSize outputSize;
    bool locked = false;
};

} // namespace UI
} // namespace Acheron

#endif // Q_OS_WIN
