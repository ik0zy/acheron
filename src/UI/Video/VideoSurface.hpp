#pragma once

#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "Core/Media/VideoDecoder.hpp"

class QPainter;

namespace Acheron {
namespace UI {

// Shows one decoded video. Where the machine can, the picture is converted and scaled by the
// graphics card, from textures it is uploaded as or, when the decoder left it on the card,
// from where it already is. Otherwise it is converted and scaled to the widget's size on the
// decoder's thread, so that painting is a copy of pixels already at the right size.
class VideoSurface : public QWidget
{
    Q_OBJECT
public:
    explicit VideoSurface(QWidget *parent = nullptr);
    ~VideoSurface() override;

    // Whether new surfaces draw through the graphics card when they can. A setting; on unless
    // the user turns it off.
    [[nodiscard]] static bool gpuDrawingEnabled();
    static void setGpuDrawingEnabled(bool enabled);

    [[nodiscard]] std::shared_ptr<Core::Media::VideoFrameSink> sink() const;
    // Whether this surface draws through the graphics card.
    [[nodiscard]] bool drawsWithGpu() const { return glLayer != nullptr; }
    // Whether the picture now showing went from the decoder to the screen without leaving the
    // graphics card.
    [[nodiscard]] bool drawsFromCard() const;

    // Text over the picture: a state such as "Stream Paused" in the middle, and statistics in
    // the corner. Empty strings clear them.
    void setMessage(const QString &title, const QString &detail = {});
    void setStatsText(const QString &text);
    // A short label in the bottom corner, such as whose video this is.
    void setCaption(const QString &text);
    [[nodiscard]] QString caption() const { return captionText; }
    // What stands in for the picture while there is none: the person's avatar.
    void setPlaceholder(const QPixmap &avatar);
    // A ring around the tile while the person is speaking.
    void setSpeaking(bool speaking);
    // The red LIVE marker of someone who is streaming.
    void setLive(bool live);

    // Sends geometryChanged again, for a listener that was not there the first time.
    void announceGeometry();

    // Forgets the picture, so that a stream which is over does not leave its last frame up.
    void clear();

signals:
    // The area the picture covers, in device pixels, and whether any of it is on screen.
    void geometryChanged(int pixelCount, bool visible);
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    class Sink;
    class GlLayer;

    void onFrameArrived();
    void reportGeometry(bool evenIfUnchanged = false);
    void refresh();
    void refreshOverlays();
    // Everything drawn on top of the picture, the same whichever way the picture is drawn.
    void paintOverlays(QPainter &painter, bool hasPicture);

    std::shared_ptr<Sink> frameSink;
    GlLayer *glLayer = nullptr;
    QTimer rescaleTimer;
    QString messageTitle;
    QString messageDetail;
    QString statsText;
    QString captionText;
    QPixmap placeholder;
    bool speaking = false;
    bool live = false;
    int reportedPixels = -1;
    bool reportedVisible = false;
};

} // namespace UI
} // namespace Acheron
