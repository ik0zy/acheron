#include "StreamWindow.hpp"

#include "UI/Video/VideoSurface.hpp"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

using Core::StreamSession;

namespace {

constexpr int MAX_VOLUME_PERCENT = 200;

} // namespace

StreamWindow::StreamWindow(StreamSession *session, const QString &streamerName, QWidget *parent)
    : QWidget(parent, Qt::Window), session(session), key(session->key().toString()), streamer(streamerName)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(streamerName);
    resize(960, 600);

    surface = new VideoSurface(this);
    session->setSink(surface->sink());
    connect(surface, &VideoSurface::geometryChanged, session, &StreamSession::setSurface);
    connect(surface, &VideoSurface::doubleClicked, this, &StreamWindow::toggleFullScreen);

    controls = new QWidget(this);
    auto *bar = new QHBoxLayout(controls);
    bar->setContentsMargins(8, 4, 8, 4);
    bar->setSpacing(8);

    viewerLabel = new QLabel(controls);
    bar->addWidget(viewerLabel);
    bar->addStretch(1);

    bar->addWidget(new QLabel(tr("Stream Volume"), controls));
    volumeSlider = new QSlider(Qt::Horizontal, controls);
    volumeSlider->setRange(0, MAX_VOLUME_PERCENT);
    volumeSlider->setValue(qRound(session->volume() * 100));
    volumeSlider->setFixedWidth(140);
    bar->addWidget(volumeSlider);
    volumeValue = new QLabel(controls);
    volumeValue->setMinimumWidth(volumeValue->fontMetrics().horizontalAdvance(QStringLiteral("200%")));
    volumeValue->setText(QStringLiteral("%1%").arg(volumeSlider->value()));
    bar->addWidget(volumeValue);
    connect(volumeSlider, &QSlider::valueChanged, this, [this](int percent) {
        volumeValue->setText(QStringLiteral("%1%").arg(percent));
        if (this->session)
            this->session->setVolume(static_cast<float>(percent) / 100.0f);
    });

    statsButton = new QPushButton(tr("Stats"), controls);
    statsButton->setCheckable(true);
    bar->addWidget(statsButton);
    connect(statsButton, &QPushButton::toggled, this, &StreamWindow::applyStats);

    fullScreenButton = new QPushButton(tr("Full Screen"), controls);
    bar->addWidget(fullScreenButton);
    connect(fullScreenButton, &QPushButton::clicked, this, &StreamWindow::toggleFullScreen);

    stopButton = new QPushButton(tr("Stop Watching"), controls);
    bar->addWidget(stopButton);
    connect(stopButton, &QPushButton::clicked, this, &QWidget::close);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(surface, 1);
    layout->addWidget(controls);

    connect(session, &StreamSession::stateChanged, this, &StreamWindow::applyState);
    connect(session, &StreamSession::statsChanged, this, &StreamWindow::applyStats);
    connect(session, &StreamSession::viewersChanged, this, &StreamWindow::applyViewers);
    connect(session, &QObject::destroyed, this, &QWidget::close);

    applyState();
    applyViewers();
}

void StreamWindow::applyState()
{
    if (!session)
        return;

    switch (session->state()) {
    case StreamSession::State::Connecting:
        surface->setMessage(tr("Connecting..."));
        break;
    case StreamSession::State::Active:
        surface->setMessage({});
        break;
    case StreamSession::State::Paused:
        surface->setMessage(tr("Stream Paused"), tr("%1 has minimized their application... sit tight!").arg(streamer));
        break;
    case StreamSession::State::Reconnecting:
        surface->setMessage(tr("You've been disconnected..."), tr("Sit tight, we're patching you back in!"));
        break;
    case StreamSession::State::Ended:
        surface->clear();
        surface->setMessage(tr("This stream has ended."));
        break;
    case StreamSession::State::Failed:
        surface->clear();
        surface->setMessage(tr("Stream failed to start :("));
        break;
    case StreamSession::State::Full:
        surface->clear();
        surface->setMessage(tr("Stream is Full"), tr("Sorry, this stream has the max number of viewers."));
        break;
    }

    stopButton->setText(session->isOver() ? tr("Close Stream") : tr("Stop Watching"));
}

void StreamWindow::applyStats()
{
    if (!session || !statsButton->isChecked()) {
        surface->setStatsText({});
        return;
    }

    const StreamSession::Stats stats = session->stats();
    QStringList lines;
    lines << tr("Codec: %1 (%2)").arg(stats.network.codec, stats.decoder.decoderName);
    lines << tr("Resolution: %1x%2").arg(stats.decoder.width).arg(stats.decoder.height);
    lines << tr("Frame rate: %1 fps").arg(stats.framesPerSecond, 0, 'f', 1);
    lines << tr("Bitrate: %1 kbps").arg(stats.bitrateKbps, 0, 'f', 0);
    lines << tr("Decode time: %1 ms").arg(stats.decoder.averageDecodeMs, 0, 'f', 2);
    lines << tr("Loss: %1%").arg(stats.network.lossFraction * 100.0, 0, 'f', 1);
    lines << tr("Packets: %1, retransmitted %2").arg(stats.network.packets).arg(stats.network.retransmitted);
    lines << tr("NACK: %1, PLI: %2").arg(stats.network.nacked).arg(stats.network.keyframeRequests);
    lines << tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoder.decoded).arg(stats.decoder.dropped).arg(stats.decoder.errors);
    if (stats.network.decryptFailures != 0)
        lines << tr("Decrypt failures: %1").arg(stats.network.decryptFailures);
    surface->setStatsText(lines.join(QLatin1Char('\n')));
}

void StreamWindow::applyViewers()
{
    if (!session)
        return;
    viewerLabel->setText(tr("Spectators - %1").arg(session->viewerIds().size()));
    emit viewersNeeded();
}

void StreamWindow::setViewerNames(const QStringList &names)
{
    viewerLabel->setToolTip(names.join(QLatin1Char('\n')));
}

void StreamWindow::toggleFullScreen()
{
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
}

void StreamWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange)
        return;
    controls->setVisible(!isFullScreen());
    fullScreenButton->setText(isFullScreen() ? tr("Exit Full Screen") : tr("Full Screen"));
}

void StreamWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        showNormal();
        return;
    }
    if (event->key() == Qt::Key_F11 || event->key() == Qt::Key_F) {
        toggleFullScreen();
        return;
    }
    QWidget::keyPressEvent(event);
}

void StreamWindow::closeEvent(QCloseEvent *event)
{
    emit stopRequested(key);
    event->accept();
}

} // namespace UI
} // namespace Acheron
