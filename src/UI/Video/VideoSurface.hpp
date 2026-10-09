#pragma once

#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "Core/Media/VideoDecoder.hpp"

namespace Acheron {
namespace UI {

// Shows one decoded video. This is the software path: frames are converted and scaled to the
// widget's size on the decoder's thread, so painting is a copy of pixels already at the right
// size.
class VideoSurface : public QWidget
{
    Q_OBJECT
public:
    explicit VideoSurface(QWidget *parent = nullptr);
    ~VideoSurface() override;

    [[nodiscard]] std::shared_ptr<Core::Media::VideoFrameSink> sink() const;

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

    void onFrameArrived();
    void reportGeometry();

    std::shared_ptr<Sink> frameSink;
    QTimer rescaleTimer;
    QString messageTitle;
    QString messageDetail;
    QString statsText;
    QString captionText;
    QPixmap placeholder;
    bool speaking = false;
    bool live = false;
};

} // namespace UI
} // namespace Acheron
