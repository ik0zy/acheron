#include "CallView.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Streams/StreamWatcher.hpp"
#include "Core/Video/CallVideo.hpp"
#include "UI/ElidedLabel.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
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
constexpr int MIN_STAGE_HEIGHT = 60;
constexpr QSize POPPED_OUT_SIZE(960, 600);
constexpr QSize TILE_WINDOW_SIZE(640, 360);

const char *const COLLAPSED_WITHOUT_VIDEO_KEY = "video/call_view_collapsed_without_video";
const char *const GEOMETRY_KEY = "video/call_view_geometry";

QString streamTileId(Snowflake userId)
{
    return QStringLiteral("stream:%1").arg(static_cast<quint64>(userId));
}

QString drawingName(const VideoSurface *surface)
{
    if (!surface->drawsWithGpu())
        return CallView::tr("software");
    return surface->drawsFromCard() ? CallView::tr("graphics card, no copy") : CallView::tr("graphics card");
}

QString userTileId(Snowflake userId)
{
    return QStringLiteral("user:%1").arg(static_cast<quint64>(userId));
}

void paintBlack(QWidget *widget)
{
    QPalette black = widget->palette();
    black.setColor(QPalette::Window, Qt::black);
    widget->setPalette(black);
    widget->setAutoFillBackground(true);
}

// One tile in a window of its own. Closing the window puts the tile back.
class TileWindow : public QWidget
{
public:
    TileWindow(VideoSurface *surface, const QString &title, std::function<void()> closed)
        : QWidget(nullptr, Qt::Window), closed(std::move(closed))
    {
        setWindowTitle(title);
        paintBlack(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(surface);
        resize(TILE_WINDOW_SIZE);
    }

    void toggleFullScreen()
    {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->accept();
        QTimer::singleShot(0, this, closed);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape && isFullScreen())
            showNormal();
        else if (event->key() == Qt::Key_F11 || event->key() == Qt::Key_F)
            toggleFullScreen();
        else
            QWidget::keyPressEvent(event);
    }

private:
    std::function<void()> closed;
};

} // namespace

CallView::CallView(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, Core::ImageManager *images, Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *dock)
    : QWidget(dock), voice(voice), watcher(watcher), video(voice->video()), images(images), accountId(accountId), names(std::move(names)), avatars(std::move(avatars)), dock(dock)
{
    setWindowTitle(title);
    collapsed = QSettings().value(COLLAPSED_WITHOUT_VIDEO_KEY, true).toBool();

    controls = new QWidget(this);
    auto *bar = new QHBoxLayout(controls);
    bar->setContentsMargins(4, 2, 4, 2);
    bar->setSpacing(8);

    collapseButton = new QToolButton(controls);
    collapseButton->setAutoRaise(true);
    bar->addWidget(collapseButton);
    connect(collapseButton, &QToolButton::clicked, this, &CallView::toggleCollapsedByHand);

    summaryLabel = new ElidedLabel(title, controls);
    bar->addWidget(summaryLabel, 1);

    nonVideoCheck = new QCheckBox(tr("Show Non-Video Participants"), controls);
    nonVideoCheck->setChecked(QSettings().value(settingsKey(), true).toBool());
    bar->addWidget(nonVideoCheck);
    connect(nonVideoCheck, &QCheckBox::toggled, this, [this](bool shown) {
        QSettings().setValue(settingsKey(), shown);
        rebuild();
    });

    statsButton = new QPushButton(tr("Stats"), controls);
    statsButton->setCheckable(true);
    bar->addWidget(statsButton);
    connect(statsButton, &QPushButton::toggled, this, &CallView::setStatsShown);

    popOutButton = new QPushButton(controls);
    bar->addWidget(popOutButton);
    connect(popOutButton, &QPushButton::clicked, this, [this] { setPoppedOut(!poppedOut); });

    fullScreenButton = new QPushButton(controls);
    bar->addWidget(fullScreenButton);
    connect(fullScreenButton, &QPushButton::clicked, this, &CallView::toggleFullScreen);

    stage = new QWidget(this);
    paintBlack(stage);
    stage->setMinimumHeight(MIN_STAGE_HEIGHT);
    stage->installEventFilter(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(controls);
    layout->addWidget(stage, 1);

    if (dock && dock->layout())
        dock->layout()->addWidget(this);

    statsTimer.setInterval(1000);
    connect(&statsTimer, &QTimer::timeout, this, &CallView::refreshStats);

    connect(voice, &Core::Audio::VoiceManager::participantJoined, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantLeft, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantUpdated, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantsCleared, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantSpeakingChanged, this, [this](Snowflake userId, bool speaking) {
        auto it = tiles.find(userTileId(userId));
        if (it != tiles.end())
            it->surface->setSpeaking(speaking);
    });
    connect(video, &Core::CallVideo::sendersChanged, this, &CallView::rebuild);
    // Not at once: it may come from a tile's own menu, and takes the tile away.
    connect(video, &Core::CallVideo::videoDisabledChanged, this, &CallView::rebuild, Qt::QueuedConnection);
    connect(watcher, &Core::StreamWatcher::sessionOpened, this, [this](StreamSession *session) {
        rebuild();
        // A stream just asked for is what the viewer wants to look at.
        if (focused.isEmpty() || focused.startsWith(QLatin1String("stream:")))
            setFocusedTile(streamTileId(session->key().ownerId));
    });
    connect(watcher, &Core::StreamWatcher::sessionClosed, this, &CallView::rebuild, Qt::QueuedConnection);
    if (images)
        connect(images, &Core::ImageManager::imageFetched, this, &CallView::onImageFetched);

    applyChrome();
    rebuild();
}

CallView::~CallView()
{
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!it->stream && video)
            video->removeTile(it->userId);
        if (it->stream && it->session)
            it->session->setSurface(0, false);
        // Takes its surface with it. The others go with the stage.
        delete it->popout.data();
    }
    if (video)
        video->setFocused(Snowflake::Invalid);
}

QString CallView::settingsKey() const
{
    return QStringLiteral("video/show_non_video/%1").arg(static_cast<quint64>(voice ? voice->currentChannelId() : Snowflake(0)));
}

Discord::StreamKey CallView::streamKeyFor(Snowflake userId) const
{
    return Discord::StreamKey::forChannel(voice->currentGuildId(), voice->currentChannelId(), userId);
}

QStringList CallView::tileIds() const
{
    return tiles.keys();
}

bool CallView::isTilePoppedOut(const QString &tileId) const
{
    const auto tile = tiles.constFind(tileId);
    return tile != tiles.constEnd() && tile->popout;
}

void CallView::removeTile(const QString &tileId)
{
    const Tile tile = tiles.take(tileId);
    if (!tile.stream && tile.camera && video)
        video->removeTile(tile.userId);
    if (tile.stream && tile.session)
        tile.session->setSurface(0, false);
    delete tile.surface;
    delete tile.popout.data();
    if (focused == tileId)
        focused.clear();
}

void CallView::rebuild()
{
    if (!voice || !watcher)
        return;

    const QList<Core::Audio::VoiceParticipant> participants = voice->currentParticipants();
    const QList<Snowflake> cameras = video ? video->senders() : QList<Snowflake>();

    const auto showsCamera = [this, &cameras](Snowflake userId) { return cameras.contains(userId) && !video->isVideoDisabled(userId); };

    int videoCount = 0;
    QStringList participantNames;
    for (const Core::Audio::VoiceParticipant &participant : participants) {
        if (participant.userId == accountId)
            continue;
        participantNames.append(names ? names(participant.userId) : QString::number(static_cast<quint64>(participant.userId)));
        videoCount += (participant.selfStream ? 1 : 0) + (showsCamera(participant.userId) ? 1 : 0);
    }
    const bool anyVideo = videoCount > 0;
    summaryLabel->setText(callSummary(participantNames, videoCount));

    const bool videoStarted = anyVideo && !videoPresent;
    videoPresent = anyVideo;
    if (videoStarted && !collapsedByHand)
        collapsed = false;
    applyChrome();

    QHash<QString, QPair<Snowflake, bool>> wanted;
    // The bar alone, over a call without video, has no use for tiles to draw on.
    const bool tilesShown = anyVideo || poppedOut || !collapsed;
    for (const Core::Audio::VoiceParticipant &participant : participants) {
        if (participant.userId == accountId || !tilesShown)
            continue;
        if (participant.selfStream)
            wanted.insert(streamTileId(participant.userId), { participant.userId, true });
        // A camera whose video has been disabled has no tile at all. It is turned back on
        // from the person's entry in the channel list.
        const bool disabled = cameras.contains(participant.userId) && !showsCamera(participant.userId);
        if (!disabled && (showsCamera(participant.userId) || nonVideoCheck->isChecked() || !anyVideo))
            wanted.insert(userTileId(participant.userId), { participant.userId, false });
    }

    const QStringList existing = tiles.keys();
    for (const QString &id : existing) {
        if (!wanted.contains(id))
            removeTile(id);
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
        connect(tile.surface, &VideoSurface::clicked, this, [this, id] { onTileClicked(id); });
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
    updatePresence();
}

QString CallView::callSummary(const QStringList &participantNames, int videoCount) const
{
    QStringList parts{ windowTitle() };
    if (!participantNames.isEmpty())
        parts.append(participantNames.join(QStringLiteral(", ")));
    if (videoCount > 0)
        parts.append(tr("%n with video", nullptr, videoCount));
    return parts.join(QStringLiteral(" \u00b7 "));
}

void CallView::updatePresence()
{
    // With nothing left to show, a window of its own has no reason to stay up.
    if (!videoPresent && poppedOut) {
        setPoppedOut(false);
        return;
    }
    show();
    emit presenceChanged();
}

void CallView::refreshTile(Tile &tile)
{
    const QString name = names ? names(tile.userId) : QString::number(static_cast<quint64>(tile.userId));
    requestAvatar(tile);

    if (tile.stream) {
        tile.surface->setCaption(name);
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

    tile.surface->setMessage({});
}

void CallView::bindSession(Tile &tile, StreamSession *session)
{
    tile.session = session;
    tile.surface->clear();
    if (!session)
        return;

    const QString id = streamTileId(tile.userId);
    // The surface is the context, so this goes when the tile does.
    connect(session, &StreamSession::stateChanged, tile.surface, [this, id] {
        auto found = tiles.find(id);
        if (found != tiles.end())
            applySessionState(found.value());
        layoutTiles();
    });

    session->setSink(tile.surface->sink());
    tile.surface->announceGeometry();
}

void CallView::applySessionState(Tile &tile)
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

int CallView::rank(const Tile &tile) const
{
    if (tile.stream)
        return tile.session && !tile.session->isOver() ? 0 : 2;
    return tile.camera ? 1 : 3;
}

void CallView::layoutTiles()
{
    // Watched streams, then cameras, then streams not being watched, then everyone else;
    // by name within each. A tile in its own window has no place here.
    QStringList order;
    for (auto it = tiles.constBegin(); it != tiles.constEnd(); ++it) {
        if (!it->popout)
            order.append(it.key());
    }
    if (order.isEmpty())
        return;
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

    if (order.contains(focused)) {
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

void CallView::setFocusedTile(const QString &tileId)
{
    const auto wanted = tiles.constFind(tileId);
    focused = wanted != tiles.constEnd() && !wanted->popout ? tileId : QString();
    if (video) {
        const auto tile = tiles.constFind(focused);
        video->setFocused(tile != tiles.constEnd() && !tile->stream ? tile->userId : Snowflake::Invalid);
    }
    layoutTiles();
}

void CallView::onTileClicked(const QString &tileId)
{
    const auto tile = tiles.constFind(tileId);
    if (tile == tiles.constEnd() || !watcher)
        return;

    // A stream not being watched is watched.
    if (tile->stream && (!tile->session || tile->session->isOver())) {
        watchClickTile = tileId;
        watchClickTimer.start();
        watcher->watch(streamKeyFor(tile->userId));
    }
}

void CallView::onTileDoubleClicked(const QString &tileId)
{
    const auto tile = tiles.constFind(tileId);
    if (tile == tiles.constEnd())
        return;
    if (tileId == watchClickTile && watchClickTimer.isValid() && watchClickTimer.elapsed() < QApplication::doubleClickInterval())
        return;

    if (tile->popout) {
        static_cast<TileWindow *>(tile->popout.data())->toggleFullScreen();
        return;
    }
    setFocusedTile(focused == tileId ? QString() : tileId);
}

void CallView::setTilePoppedOut(const QString &tileId, bool out)
{
    auto tile = tiles.find(tileId);
    if (tile == tiles.end() || out == !tile->popout.isNull())
        return;

    if (out) {
        auto *window = new TileWindow(tile->surface, tile->surface->caption(), [this, tileId] { setTilePoppedOut(tileId, false); });
        tile->popout = window;
        tile->surface->show();
        window->show();
    } else {
        QWidget *window = tile->popout;
        tile->popout = nullptr;
        tile->surface->setParent(stage);
        window->hide();
        window->deleteLater();
    }

    if (focused == tileId)
        setFocusedTile({});
    else
        layoutTiles();
}

void CallView::showTileMenu(const QString &tileId, const QPoint &globalPos)
{
    auto found = tiles.constFind(tileId);
    if (found == tiles.constEnd() || !watcher)
        return;
    const Tile tile = found.value();

    QMenu menu(this);
    QAction *watch = nullptr;
    QAction *stop = nullptr;
    QAction *disable = nullptr;
    QAction *focus = nullptr;

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
    }

    if (!tile.popout) {
        focus = menu.addAction(tr("Focus"));
        focus->setCheckable(true);
        focus->setChecked(focused == tileId);
    }
    QAction *popOut = menu.addAction(tr("Pop Out"));
    popOut->setCheckable(true);
    popOut->setChecked(!tile.popout.isNull());

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
        video->setVideoDisabled(tile.userId, true);
    } else if (chosen == focus) {
        setFocusedTile(focus->isChecked() ? tileId : QString());
    } else if (chosen == popOut) {
        setTilePoppedOut(tileId, popOut->isChecked());
    }
}

void CallView::setStatsShown(bool shown)
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

void CallView::refreshStats()
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
            if (stats.network.serverEstimateBps != 0)
                lines << tr("Server estimate: %1 kbps, %2% loss").arg(stats.network.serverEstimateBps / 1000).arg(stats.network.serverLossFraction * 100.0, 0, 'f', 1);
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

void CallView::requestAvatar(Tile &tile)
{
    if (!images || !avatars)
        return;
    const QUrl url = avatars(tile.userId);
    if (!url.isValid())
        return;
    const QString id = tile.stream ? streamTileId(tile.userId) : userTileId(tile.userId);
    tile.surface->setPlaceholder(avatarTracker.fetch(images, url, AVATAR_SIZE, id, accountId));
}

void CallView::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_SIZE)
        return;
    avatarTracker.notify(url, [this, &pixmap](const QString &id) {
        auto tile = tiles.constFind(id);
        if (tile != tiles.constEnd())
            tile->surface->setPlaceholder(pixmap);
    });
}

void CallView::popOut(bool fullScreen)
{
    poppedOut = true;
    setParent(nullptr, Qt::Window);
    const QByteArray geometry = QSettings().value(GEOMETRY_KEY).toByteArray();
    if (geometry.isEmpty() || !restoreGeometry(geometry))
        resize(POPPED_OUT_SIZE);
    applyChrome();
    if (fullScreen)
        showFullScreen();
    else
        show();
    raise();
    activateWindow();
    emit presenceChanged();
}

void CallView::setPoppedOut(bool out)
{
    if (out == poppedOut)
        return;
    if (out) {
        popOut(false);
        return;
    }

    if (isFullScreen())
        showNormal();
    else
        QSettings().setValue(GEOMETRY_KEY, saveGeometry());
    poppedOut = false;
    dockAfterFullScreen = false;
    if (!dock || !dock->layout()) {
        // The main window has gone, and with it anywhere to go back to.
        deleteLater();
        return;
    }
    dock->layout()->addWidget(this);
    show();
    rebuild();
}

void CallView::setCollapsed(bool value)
{
    if (collapsed == value)
        return;
    collapsed = value;
    rebuild();
}

void CallView::toggleCollapsedByHand()
{
    collapsedByHand = !collapsed;
    if (!videoPresent)
        QSettings().setValue(COLLAPSED_WITHOUT_VIDEO_KEY, collapsedByHand);
    setCollapsed(collapsedByHand);
}

void CallView::reveal()
{
    if (!poppedOut) {
        collapsedByHand = false;
        setCollapsed(false);
        return;
    }
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

// What is shown of the view's own chrome depends on where it is: docked it can be collapsed
// to its bar, and full screen it has no bar at all.
void CallView::applyChrome()
{
    const bool fullScreen = poppedOut && isFullScreen();
    const bool barOnly = !poppedOut && collapsed;
    controls->setVisible(!fullScreen);
    collapseButton->setVisible(!poppedOut);
    nonVideoCheck->setVisible(videoPresent && !barOnly);
    statsButton->setVisible(videoPresent && !barOnly);
    popOutButton->setVisible(videoPresent || poppedOut);
    fullScreenButton->setVisible(videoPresent || poppedOut);
    collapseButton->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
    popOutButton->setText(poppedOut ? tr("Dock") : tr("Pop Out"));
    fullScreenButton->setText(fullScreen ? tr("Exit Full Screen") : tr("Full Screen"));
    stage->setVisible(!barOnly);
    setMaximumHeight(barOnly ? controls->sizeHint().height() : QWIDGETSIZE_MAX);
}

void CallView::toggleFullScreen()
{
    // Full screen is a state of a window, so a docked view goes out for it and comes back after.
    if (!poppedOut) {
        popOut(true);
        dockAfterFullScreen = true;
        return;
    }
    if (!isFullScreen()) {
        showFullScreen();
        return;
    }
    if (dockAfterFullScreen)
        setPoppedOut(false);
    else
        showNormal();
}

bool CallView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == stage && event->type() == QEvent::Resize)
        layoutTiles();
    return QWidget::eventFilter(watched, event);
}

void CallView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        applyChrome();
}

void CallView::keyPressEvent(QKeyEvent *event)
{
    // Only in its own window: docked, these keys are the main window's.
    if (poppedOut && event->key() == Qt::Key_Escape && isFullScreen()) {
        toggleFullScreen();
        return;
    }
    if (poppedOut && (event->key() == Qt::Key_F11 || event->key() == Qt::Key_F)) {
        toggleFullScreen();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CallView::closeEvent(QCloseEvent *event)
{
    // Only its own window can be closed, and that puts the view back in the main window.
    event->accept();
    if (poppedOut)
        QTimer::singleShot(0, this, [this] { setPoppedOut(false); });
}

} // namespace UI
} // namespace Acheron
