#pragma once

#include <QElapsedTimer>
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
class QLabel;
class QMenu;
class QPushButton;
class QToolButton;

namespace Acheron {
namespace Core {
class CallVideo;
class StreamBroadcaster;
class StreamWatcher;
namespace Audio {
class VoiceManager;
}
namespace Video {
struct SendQuality;
}
} // namespace Core

namespace UI {

class ElidedLabel;
class VideoSurface;

// The call an account is in: a tile for each stream, each camera and, optionally, everyone
// else. It sits in the main window, in `dock`, for as long as the call lasts, and can be
// collapsed to its bar there. A call without video starts as the bar alone and opens when
// video starts, unless the viewer collapsed it by hand during the call. The whole of it, or
// any one tile, can be moved out into a window of its own and back.
//
// Clicking a stream that is not being watched watches it. Double-clicking a tile enlarges it
// over a strip of the others.
//
// The account's own screen share is started and stopped from the bar, and has a tile like any
// other stream, drawn from the capture's previews. A share that ended by itself leaves its
// tile up to say why, until it is clicked away.
class CallView : public QWidget
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    // `dock` is the widget whose layout the view lives in while it is not in its own window.
    CallView(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, Core::StreamBroadcaster *broadcaster, Core::ImageManager *images, Core::Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *dock);
    ~CallView() override;

    // Enlarges one tile, or with an empty id goes back to the grid. Ids are "stream:<user>"
    // and "user:<user>".
    void setFocusedTile(const QString &tileId);
    [[nodiscard]] QString focusedTile() const { return focused; }
    [[nodiscard]] QStringList tileIds() const;
    void setStatsShown(bool shown);

    // Whether anyone in the call has a camera on or a stream up.
    [[nodiscard]] bool hasVideo() const { return videoPresent; }

    [[nodiscard]] bool isPoppedOut() const { return poppedOut; }
    void setPoppedOut(bool out);
    [[nodiscard]] bool isTilePoppedOut(const QString &tileId) const;
    void setTilePoppedOut(const QString &tileId, bool out);

    [[nodiscard]] bool isCollapsed() const { return collapsed; }
    void setCollapsed(bool value);
    // Brings the view into sight: opened up where it is docked, or its window raised.
    void reveal();

    static constexpr int MAX_STRIP_TILES = 12;
    static constexpr QSize AVATAR_SIZE = QSize(128, 128);

signals:
    // Whether the view takes up room in its dock has changed.
    void presenceChanged();

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
        // The tile's own window, while it is out of the view.
        QPointer<QWidget> popout;
    };

    void rebuild();
    void refreshTile(Tile &tile);
    void bindSession(Tile &tile, Core::StreamSession *session);
    void applySessionState(Tile &tile);
    void applyOwnShareState(Tile &tile);
    void showOwnPreview(const QImage &preview);
    void onShareClicked();
    void fillQualityMenu(QMenu *menu);
    void changeShareQuality(const Core::Video::SendQuality &quality);
    [[nodiscard]] QStringList ownShareStats() const;
    [[nodiscard]] bool isOwnShare(const Tile &tile) const { return tile.stream && tile.userId == accountId; }
    [[nodiscard]] bool ownShareShown() const;
    void layoutTiles();
    void refreshStats();
    void showTileMenu(const QString &tileId, const QPoint &globalPos);
    void onTileClicked(const QString &tileId);
    void onTileDoubleClicked(const QString &tileId);
    void removeTile(const QString &tileId);
    void popOut(bool fullScreen);
    void toggleFullScreen();
    void applyChrome();
    void toggleCollapsedByHand();
    [[nodiscard]] QString callSummary(const QStringList &participantNames, int videoCount) const;
    void updatePresence();
    void requestAvatar(Tile &tile);
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

    [[nodiscard]] Discord::StreamKey streamKeyFor(Core::Snowflake userId) const;
    [[nodiscard]] int rank(const Tile &tile) const;
    [[nodiscard]] QString settingsKey() const;

    QPointer<Core::Audio::VoiceManager> voice;
    QPointer<Core::StreamWatcher> watcher;
    QPointer<Core::StreamBroadcaster> broadcaster;
    QPointer<Core::CallVideo> video;
    Core::ImageManager *images;
    Core::Snowflake accountId;
    NameResolver names;
    AvatarResolver avatars;
    AvatarRequestTracker<QString> avatarTracker;

    QHash<QString, Tile> tiles;
    QString focused;
    // The click that started watching a stream, so that the second click of a double click
    // on it is not also taken as a wish to shrink it again.
    QString watchClickTile;
    QElapsedTimer watchClickTimer;

    QPointer<QWidget> dock;
    bool videoPresent = false;
    bool poppedOut = false;
    bool collapsed = false;
    bool collapsedByHand = false;
    bool dockAfterFullScreen = false;

    QWidget *stage;
    QWidget *controls;
    QToolButton *collapseButton;
    ElidedLabel *summaryLabel;
    QToolButton *shareButton;
    QMenu *shareQualityMenu;
    bool captureAvailable = false;
    QCheckBox *nonVideoCheck;
    QPushButton *statsButton;
    QPushButton *popOutButton;
    QPushButton *fullScreenButton;
    QTimer statsTimer;
};

} // namespace UI
} // namespace Acheron
