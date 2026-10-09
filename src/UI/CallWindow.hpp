#pragma once

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <functional>

#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamSession.hpp"
#include "UI/AvatarRequestTracker.hpp"

class QCheckBox;
class QPushButton;

namespace Acheron {
namespace Core {
class CallVideo;
class StreamWatcher;
namespace Audio {
class VoiceManager;
}
} // namespace Core

namespace UI {

class VideoSurface;

// The video side of the call an account is in: a tile for each stream, each camera and,
// optionally, everyone else. Double-clicking a tile enlarges it over a strip of the others.
// Closing the window stops watching.
class CallWindow : public QWidget
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    CallWindow(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, Core::ImageManager *images, Core::Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *parent = nullptr);
    ~CallWindow() override;

    // Enlarges one tile, or with an empty id goes back to the grid. Ids are "stream:<user>"
    // and "user:<user>".
    void setFocusedTile(const QString &tileId);
    [[nodiscard]] QString focusedTile() const { return focused; }
    [[nodiscard]] QStringList tileIds() const;
    void setStatsShown(bool shown);

    static constexpr int MAX_STRIP_TILES = 12;
    static constexpr QSize AVATAR_SIZE = QSize(128, 128);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct Tile
    {
        Core::Snowflake userId;
        bool stream = false;
        VideoSurface *surface = nullptr;
        QPointer<Core::StreamSession> session;
        bool camera = false;
    };

    void rebuild();
    void refreshTile(Tile &tile);
    void bindSession(Tile &tile, Core::StreamSession *session);
    void applySessionState(Tile &tile);
    void layoutTiles();
    void refreshStats();
    void showTileMenu(const QString &tileId, const QPoint &globalPos);
    void onTileDoubleClicked(const QString &tileId);
    void toggleFullScreen();
    void requestAvatar(Tile &tile);
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

    [[nodiscard]] Discord::StreamKey streamKeyFor(Core::Snowflake userId) const;
    [[nodiscard]] int rank(const Tile &tile) const;
    [[nodiscard]] QString settingsKey() const;

    QPointer<Core::Audio::VoiceManager> voice;
    QPointer<Core::StreamWatcher> watcher;
    QPointer<Core::CallVideo> video;
    Core::ImageManager *images;
    Core::Snowflake accountId;
    NameResolver names;
    AvatarResolver avatars;
    AvatarRequestTracker<QString> avatarTracker;

    QHash<QString, Tile> tiles;
    QString focused;

    QWidget *stage;
    QWidget *controls;
    QCheckBox *nonVideoCheck;
    QPushButton *statsButton;
    QPushButton *fullScreenButton;
    QTimer statsTimer;
};

} // namespace UI
} // namespace Acheron
