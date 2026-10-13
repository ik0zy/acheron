#pragma once

#include <QtGlobal>

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)

#define ACHERON_HAVE_DMABUF 1

#include <memory>

struct AVFrame;

namespace Acheron {
namespace UI {

// Makes a VAAPI picture's planes OpenGL textures through EGL_EXT_image_dma_buf_import, copying
// nothing. exported() is for any thread; the rest needs the OpenGL context current.
class DmabufGlBridge
{
public:
    // Whether the current OpenGL context can have a DMA-BUF for a texture at all. It cannot
    // once EGL has refused one: that is not asked again while the program runs.
    [[nodiscard]] static bool contextSupportsIt();

    // The VAAPI frame as DMA-BUFs, of the size of its whole surface, for the caller to free, or
    // null. Waits until the card has finished decoding the picture.
    [[nodiscard]] static AVFrame *exported(const AVFrame *frame);

    DmabufGlBridge();
    ~DmabufGlBridge();
    DmabufGlBridge(const DmabufGlBridge &) = delete;
    DmabufGlBridge &operator=(const DmabufGlBridge &) = delete;

    // Makes the textures the planes of an exported 8-bit 4:2:0 frame: brightness in red, the
    // colour pair in red and green. False when this machine cannot, or not for such a frame.
    bool update(const AVFrame *exported, unsigned int lumaTexture, unsigned int chromaTexture);

    // The textures are the decoder's own surface, kept from it until the next frame or this.
    void release();

private:
    struct Egl;

    std::unique_ptr<Egl> egl;
    AVFrame *held;
};

} // namespace UI
} // namespace Acheron

#endif
