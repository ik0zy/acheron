#include "CallWindow.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Streams/StreamWatcher.hpp"
#include "Core/Video/CallVideo.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace UI {

using Core::Snowflake;
using Core::StreamSession;

namespace {

constexpr int TILE_GAP = 2;
constexpr int MAX_VOLUME_PERCENT = 200;

QString streamTileId(Snowflake userId)
{
    return QStringLiteral("stream:%1").arg(static_cast<quint64>(userId));
}

QString drawingName(const VideoSurface *surface)
{
    if (!surface->drawsWithGpu())
        return CallWindow::tr("software");
    return surface->drawsFromCard() ? CallWindow::tr("graphics card, no copy") : CallWindow::tr("graphics card");
}

QString userTileId(Snowflake userId)
{
    return QStringLiteral("user:%1").arg(static_cast<quint64>(userId));
}

} // namespace

CallWindow::CallWindow(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, Core::ImageManager *images, Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), voice(voice), watcher(watcher), video(voice->video()), images(images), accountId(accountId), names(std::move(names)), avatars(std::move(avatars))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    resize(960, 600);

    stage = new QWidget(this);
    QPalette black = stage->palette();
    black.setColor(QPalette::Window, Qt::black);
    stage->setPalette(black);
    stage->setAutoFillBackground(true);
    stage->installEventFilter(this);

    controls = new QWidget(this);
    auto *bar = new QHBoxLayout(controls);
    bar->setContentsMargins(8, 4, 8, 4);
    bar->setSpacing(8);

    nonVideoCheck = new QCheckBox(tr("Show Non-Video Participants"), controls);
    nonVideoCheck->setChecked(QSettings().value(settingsKey(), true).toBool());
    bar->addWidget(nonVideoCheck);
    connect(nonVideoCheck, &QCheckBox::toggled, this, [this](bool shown) {
        QSettings().setValue(settingsKey(), shown);
        rebuild();
    });
    bar->addStretch(1);

    statsButton = new QPushButton(tr("Stats"), controls);
    statsButton->setCheckable(true);
    bar->addWidget(statsButton);
    connect(statsButton, &QPushButton::toggled, this, &CallWindow::setStatsShown);

    fullScreenButton = new QPushButton(tr("Full Screen"), controls);
    bar->addWidget(fullScreenButton);
    connect(fullScreenButton, &QPushButton::clicked, this, &CallWindow::toggleFullScreen);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(stage, 1);
    layout->addWidget(controls);

    statsTimer.setInterval(1000);
    connect(&statsTimer, &QTimer::timeout, this, &CallWindow::refreshStats);

    connect(voice, &Core::Audio::VoiceManager::participantJoined, this, &CallWindow::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantLeft, this, &CallWindow::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantUpdated, this, &CallWindow::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantsCleared, this, &CallWindow::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantSpeakingChanged, this, [this](Snowflake userId, bool speaking) {
        auto it = tiles.find(userTileId(userId));
        if (it != tiles.end())
            it->surface->setSpeaking(speaking);
    });
    connect(voice, &Core::Audio::VoiceManager::voiceDisconnected, this, &QWidget::close);
    connect(video, &Core::CallVideo::sendersChanged, this, &CallWindow::rebuild);
    connect(watcher, &Core::StreamWatcher::sessionOpened, this, [this](StreamSession *session) {
        rebuild();
        // A stream just asked for is what the viewer wants to look at.
        if (focused.isEmpty() || focused.startsWith(QLatin1String("stream:")))
            setFocusedTile(streamTileId(session->key().ownerId));
    });
    connect(watcher, &Core::StreamWatcher::sessionClosed, this, &CallWindow::rebuild, Qt::QueuedConnection);
    if (images)
        connect(images, &Core::ImageManager::imageFetched, this, &CallWindow::onImageFetched);

    rebuild();
}

CallWindow::~CallWindow()
{
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!it->stream && video)
            video->removeTile(it->userId);
        if (it->stream && it->session)
            it->session->setSurface(0, false);
    }
    if (video)
        video->setFocused(Snowflake::Invalid);
}

QString CallWindow::settingsKey() const
{
    return QStringLiteral("video/show_non_video/%1").arg(static_cast<quint64>(voice ? voice->currentChannelId() : Snowflake(0)));
}

Discord::StreamKey CallWindow::streamKeyFor(Snowflake userId) const
{
    return Discord::StreamKey::forChannel(voice->currentGuildId(), voice->currentChannelId(), userId);
}

QStringList CallWindow::tileIds() const
{
    return tiles.keys();
}

void CallWindow::rebuild()
{
    if (!voice || !watcher)
        return;

    const QList<Core::Audio::VoiceParticipant> participants = voice->currentParticipants();
    const QList<Snowflake> cameras = video ? video->senders() : QList<Snowflake>();

    QHash<QString, QPair<Snowflake, bool>> wanted;
    for (const Core::Audio::VoiceParticipant &participant : participants) {
        if (participant.userId == accountId)
            continue;
        if (participant.selfStream)
            wanted.insert(streamTileId(participant.userId), { participant.userId, true });
        if (cameras.contains(participant.userId) || nonVideoCheck->isChecked())
            wanted.insert(userTileId(participant.userId), { participant.userId, false });
    }

    const QStringList existing = tiles.keys();
    for (const QString &id : existing) {
        if (wanted.contains(id))
            continue;
        Tile tile = tiles.take(id);
        if (!tile.stream && tile.camera && video)
            video->removeTile(tile.userId);
        if (tile.stream && tile.session)
            tile.session->setSurface(0, false);
        delete tile.surface;
        if (focused == id)
            focused.clear();
    }

    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        if (tiles.contains(it.key()))
            continue;

        const QString id = it.key();
        Tile tile;
        tile.userId = it.value().first;
        tile.stream = it.value().second;
        tile.surface = new VideoSurface(stage);
        tile.surface->setMinimumSize(1, 1);
        tile.surface->setContextMenuPolicy(Qt::CustomContextMenu);
        tile.surface->show();

        connect(tile.surface, &VideoSurface::geometryChanged, this, [this, id](int pixelCount, bool visible) {
            auto found = tiles.find(id);
            if (found == tiles.end())
                return;
            if (found->stream && found->session)
                found->session->setSurface(pixelCount, visible);
            else if (!found->stream && found->camera && video)
                video->setTile(found->userId, found->surface->sink(), pixelCount, visible);
        });
        connect(tile.surface, &VideoSurface::doubleClicked, this, [this, id] { onTileDoubleClicked(id); });
        connect(tile.surface, &QWidget::customContextMenuRequested, this, [this, id](const QPoint &pos) {
            auto found = tiles.constFind(id);
            if (found != tiles.constEnd())
                showTileMenu(id, found->surface->mapToGlobal(pos));
        });
        tiles.insert(id, tile);
    }

    for (auto it = tiles.begin(); it != tiles.end(); ++it)
        refreshTile(it.value());

    if (video) {
        const auto focusedTile = tiles.constFind(focused);
        video->setFocused(focusedTile != tiles.constEnd() && !focusedTile->stream ? focusedTile->userId : Snowflake::Invalid);
    }
    layoutTiles();
}

void CallWindow::refreshTile(Tile &tile)
{
    const QString name = names ? names(tile.userId) : QString::number(static_cast<quint64>(tile.userId));
    requestAvatar(tile);

    if (tile.stream) {
        tile.surface->setCaption(name);
        tile.surface->setLive(true);
        StreamSession *session = watcher->session(streamKeyFor(tile.userId).toString());
        if (session != tile.session)
            bindSession(tile, session);
        applySessionState(tile);
        return;
    }

    const Core::Audio::VoiceParticipant *participant = voice->participant(tile.userId);
    QString caption = name;
    if (participant && (participant->selfDeaf || participant->serverDeaf))
        caption = tr("%1 (deafened)").arg(name);
    else if (participant && (participant->selfMute || participant->serverMute))
        caption = tr("%1 (muted)").arg(name);
    tile.surface->setCaption(caption);
    tile.surface->setSpeaking(participant && participant->speaking);

    const bool camera = video && video->senders().contains(tile.userId);
    if (camera != tile.camera) {
        tile.camera = camera;
        if (camera) {
            tile.surface->announceGeometry();
        } else {
            if (video)
                video->removeTile(tile.userId);
            tile.surface->clear();
        }
    }

    if (camera && video->isVideoDisabled(tile.userId)) {
        tile.surface->clear();
        tile.surface->setMessage(tr("Video Disabled"));
    } else {
        tile.surface->setMessage({});
    }
}

void CallWindow::bindSession(Tile &tile, StreamSession *session)
{
    tile.session = session;
    tile.surface->clear();
    if (!session)
        return;

    const QString id = streamTileId(tile.userId);
    // The surface is the context, so these go when the tile does.
    connect(session, &StreamSession::stateChanged, tile.surface, [this, id] {
        auto found = tiles.find(id);
        if (found != tiles.end())
            applySessionState(found.value());
        layoutTiles();
    });
    connect(session, &StreamSession::viewersChanged, tile.surface, [this, id] {
        auto found = tiles.find(id);
        if (found != tiles.end() && found->session)
            found->surface->setToolTip(tr("Spectators - %1").arg(found->session->viewerIds().size()));
    });

    session->setSink(tile.surface->sink());
    tile.surface->setToolTip(tr("Spectators - %1").arg(session->viewerIds().size()));
    tile.surface->announceGeometry();
}

void CallWindow::applySessionState(Tile &tile)
{
    VideoSurface *surface = tile.surface;
    if (!tile.session) {
        surface->setMessage(tr("Watch Stream"));
        return;
    }

    const QString name = names ? names(tile.userId) : QString::number(static_cast<quint64>(tile.userId));
    switch (tile.session->state()) {
    case StreamSession::State::Connecting:
        surface->setMessage(tr("Connecting..."));
        break;
    case StreamSession::State::Active:
        surface->setMessage({});
        break;
    case StreamSession::State::Paused:
        surface->setMessage(tr("Stream Paused"), tr("%1 has minimized their application... sit tight!").arg(name));
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
}

int CallWindow::rank(const Tile &tile) const
{
    if (tile.stream)
        return tile.session && !tile.session->isOver() ? 0 : 2;
    return tile.camera ? 1 : 3;
}

void CallWindow::layoutTiles()
{
    if (tiles.isEmpty())
        return;

    // Watched streams, then cameras, then streams not being watched, then everyone else;
    // by name within each.
    QStringList order = tiles.keys();
    std::sort(order.begin(), order.end(), [this](const QString &a, const QString &b) {
        const Tile &left = tiles[a];
        const Tile &right = tiles[b];
        if (rank(left) != rank(right))
            return rank(left) < rank(right);
        const int byName = QString::localeAwareCompare(left.surface->caption(), right.surface->caption());
        return byName != 0 ? byName < 0 : a < b;
    });

    const QRect area = stage->rect();
    const auto place = [](VideoSurface *surface, const QRect &where) {
        surface->setGeometry(where);
        surface->setVisible(true);
    };

    if (!focused.isEmpty() && tiles.contains(focused)) {
        order.removeAll(focused);
        const int stripCount = qMin(static_cast<int>(order.size()), MAX_STRIP_TILES);
        const int stripHeight = stripCount == 0 ? 0 : qBound(54, area.height() / 5, 144);
        const int largeHeight = area.height() - (stripCount == 0 ? 0 : stripHeight + TILE_GAP);
        place(tiles[focused].surface, QRect(area.left(), area.top(), area.width(), largeHeight));

        if (stripCount > 0) {
            const int widest = (area.width() - TILE_GAP * (stripCount - 1)) / stripCount;
            const int tileWidth = qMin(stripHeight * 16 / 9, widest);
            const int used = tileWidth * stripCount + TILE_GAP * (stripCount - 1);
            int x = area.left() + (area.width() - used) / 2;
            for (int i = 0; i < stripCount; i++) {
                place(tiles[order[i]].surface, QRect(x, area.top() + largeHeight + TILE_GAP, tileWidth, stripHeight));
                x += tileWidth + TILE_GAP;
            }
        }
        for (int i = stripCount; i < order.size(); i++)
            tiles[order[i]].surface->setVisible(false);
        return;
    }

    const int count = static_cast<int>(order.size());
    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    const int cellWidth = (area.width() - TILE_GAP * (columns - 1)) / columns;
    const int cellHeight = (area.height() - TILE_GAP * (rows - 1)) / rows;
    for (int i = 0; i < count; i++) {
        const int column = i % columns;
        const int row = i / columns;
        place(tiles[order[i]].surface, QRect(area.left() + column * (cellWidth + TILE_GAP), area.top() + row * (cellHeight + TILE_GAP), cellWidth, cellHeight));
    }
}

void CallWindow::setFocusedTile(const QString &tileId)
{
    focused = tiles.contains(tileId) ? tileId : QString();
    if (video) {
        const auto tile = tiles.constFind(focused);
        video->setFocused(tile != tiles.constEnd() && !tile->stream ? tile->userId : Snowflake::Invalid);
    }
    layoutTiles();
}

void CallWindow::onTileDoubleClicked(const QString &tileId)
{
    auto tile = tiles.constFind(tileId);
    if (tile == tiles.constEnd())
        return;

    // A stream not yet being watched is watched; anything else is enlarged or put back.
    if (tile->stream && (!tile->session || tile->session->isOver())) {
        watcher->watch(streamKeyFor(tile->userId));
        return;
    }
    setFocusedTile(focused == tileId ? QString() : tileId);
}

void CallWindow::showTileMenu(const QString &tileId, const QPoint &globalPos)
{
    auto found = tiles.constFind(tileId);
    if (found == tiles.constEnd() || !watcher)
        return;
    const Tile tile = found.value();

    QMenu menu(this);
    QAction *watch = nullptr;
    QAction *stop = nullptr;
    QAction *disable = nullptr;

    if (tile.stream && tile.session && !tile.session->isOver()) {
        stop = menu.addAction(tr("Stop Watching"));

        auto *volumeRow = new QWidget(&menu);
        auto *rowLayout = new QHBoxLayout(volumeRow);
        rowLayout->setContentsMargins(8, 2, 8, 2);
        rowLayout->addWidget(new QLabel(tr("Stream Volume"), volumeRow));
        auto *slider = new QSlider(Qt::Horizontal, volumeRow);
        slider->setRange(0, MAX_VOLUME_PERCENT);
        slider->setValue(qRound(tile.session->volume() * 100));
        slider->setFixedWidth(120);
        rowLayout->addWidget(slider);
        connect(slider, &QSlider::valueChanged, tile.session.data(), [session = tile.session](int percent) {
            if (session)
                session->setVolume(static_cast<float>(percent) / 100.0f);
        });
        auto *volumeAction = new QWidgetAction(&menu);
        volumeAction->setDefaultWidget(volumeRow);
        menu.addAction(volumeAction);
    } else if (tile.stream) {
        watch = menu.addAction(tile.session ? tr("Close Stream") : tr("Watch Stream"));
    } else if (tile.camera && video) {
        disable = menu.addAction(tr("Disable Video"));
        disable->setCheckable(true);
        disable->setChecked(video->isVideoDisabled(tile.userId));
    }

    QAction *focus = menu.addAction(tr("Focus"));
    focus->setCheckable(true);
    focus->setChecked(focused == tileId);

    QAction *chosen = menu.exec(globalPos);
    if (!chosen || !watcher)
        return;

    if (chosen == stop) {
        watcher->stopWatching(streamKeyFor(tile.userId).toString());
    } else if (chosen == watch) {
        if (tile.session)
            watcher->stopWatching(streamKeyFor(tile.userId).toString());
        else
            watcher->watch(streamKeyFor(tile.userId));
    } else if (chosen == disable && video) {
        video->setVideoDisabled(tile.userId, disable->isChecked());
        rebuild();
    } else if (chosen == focus) {
        setFocusedTile(focus->isChecked() ? tileId : QString());
    }
}

void CallWindow::setStatsShown(bool shown)
{
    if (statsButton->isChecked() != shown) {
        statsButton->setChecked(shown);
        return;
    }
    if (shown)
        statsTimer.start();
    else
        statsTimer.stop();
    refreshStats();
}

void CallWindow::refreshStats()
{
    const bool shown = statsButton->isChecked();
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!shown) {
            it->surface->setStatsText({});
            continue;
        }

        QStringList lines;
        if (it->stream && it->session) {
            const StreamSession::Stats stats = it->session->stats();
            lines << tr("Codec: %1 (%2)").arg(stats.network.codec, stats.decoder.decoderName);
            lines << tr("Resolution: %1x%2").arg(stats.decoder.width).arg(stats.decoder.height);
            lines << tr("Frame rate: %1 fps").arg(stats.framesPerSecond, 0, 'f', 1);
            lines << tr("Bitrate: %1 kbps").arg(stats.bitrateKbps, 0, 'f', 0);
            lines << tr("Decode time: %1 ms").arg(stats.decoder.averageDecodeMs, 0, 'f', 2);
            lines << tr("Loss: %1%").arg(stats.network.lossFraction * 100.0, 0, 'f', 1);
            lines << tr("NACK: %1, PLI: %2").arg(stats.network.nacked).arg(stats.network.keyframeRequests);
            lines << tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoder.decoded).arg(stats.decoder.dropped).arg(stats.decoder.errors);
            if (stats.network.decryptFailures != 0)
                lines << tr("Decrypt failures: %1").arg(stats.network.decryptFailures);
            lines << tr("Drawing: %1").arg(drawingName(it->surface));
        } else if (!it->stream && it->camera && video) {
            const Core::Media::VideoDecoder::Stats stats = video->decoderStats(it->userId);
            if (!stats.decoderName.isEmpty()) {
                lines << tr("Decoder: %1").arg(stats.decoderName);
                lines << tr("Resolution: %1x%2").arg(stats.width).arg(stats.height);
                lines << tr("Decode time: %1 ms").arg(stats.averageDecodeMs, 0, 'f', 2);
                lines << tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoded).arg(stats.dropped).arg(stats.errors);
                lines << tr("Drawing: %1").arg(drawingName(it->surface));
            }
        }
        it->surface->setStatsText(lines.join(QLatin1Char('\n')));
    }
}

void CallWindow::requestAvatar(Tile &tile)
{
    if (!images || !avatars)
        return;
    const QUrl url = avatars(tile.userId);
    if (!url.isValid())
        return;
    const QString id = tile.stream ? streamTileId(tile.userId) : userTileId(tile.userId);
    tile.surface->setPlaceholder(avatarTracker.fetch(images, url, AVATAR_SIZE, id, accountId));
}

void CallWindow::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_SIZE)
        return;
    avatarTracker.notify(url, [this, &pixmap](const QString &id) {
        auto tile = tiles.constFind(id);
        if (tile != tiles.constEnd())
            tile->surface->setPlaceholder(pixmap);
    });
}

void CallWindow::toggleFullScreen()
{
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
}

bool CallWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == stage && event->type() == QEvent::Resize)
        layoutTiles();
    return QWidget::eventFilter(watched, event);
}

void CallWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange)
        return;
    controls->setVisible(!isFullScreen());
    fullScreenButton->setText(isFullScreen() ? tr("Exit Full Screen") : tr("Full Screen"));
}

void CallWindow::keyPressEvent(QKeyEvent *event)
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

void CallWindow::closeEvent(QCloseEvent *event)
{
    // Out of sight, a stream would go on playing its sound and using the connection.
    if (watcher)
        watcher->stopAll();
    event->accept();
}

} // namespace UI
} // namespace Acheron
