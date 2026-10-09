#pragma once

#include <QPointer>
#include <QWidget>

#include "Core/Streams/StreamSession.hpp"

class QLabel;
class QPushButton;
class QSlider;

namespace Acheron {
namespace UI {

class VideoSurface;

// The viewer for one watched stream: the picture, what state the stream is in, and the
// controls for it. Closing the window stops watching.
class StreamWindow : public QWidget
{
    Q_OBJECT
public:
    StreamWindow(Core::StreamSession *session, const QString &streamerName, QWidget *parent = nullptr);

    [[nodiscard]] QString streamKey() const { return key; }

    // Names for the viewers, in the order the session lists them.
    void setViewerNames(const QStringList &names);

signals:
    void stopRequested(const QString &streamKey);
    void viewersNeeded();

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void applyState();
    void applyStats();
    void applyViewers();
    void toggleFullScreen();

    QPointer<Core::StreamSession> session;
    QString key;
    QString streamer;

    VideoSurface *surface;
    QWidget *controls;
    QLabel *viewerLabel;
    QSlider *volumeSlider;
    QLabel *volumeValue;
    QPushButton *statsButton;
    QPushButton *fullScreenButton;
    QPushButton *stopButton;
};

} // namespace UI
} // namespace Acheron
