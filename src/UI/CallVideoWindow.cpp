#include "CallVideoWindow.hpp"

#include "UI/Video/VideoSurface.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QMenu>

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace UI {

using Core::Snowflake;

CallVideoWindow::CallVideoWindow(Core::CallVideo *video, NameResolver names, const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), video(video), names(std::move(names))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    resize(960, 600);

    grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);

    emptyLabel = new QLabel(tr("No one has their camera on."), this);
    emptyLabel->setAlignment(Qt::AlignCenter);

    connect(video, &Core::CallVideo::sendersChanged, this, &CallVideoWindow::rebuild);
    connect(video, &QObject::destroyed, this, &QWidget::close);
    rebuild();
}

CallVideoWindow::~CallVideoWindow()
{
    if (!video)
        return;
    for (auto it = tiles.constBegin(); it != tiles.constEnd(); ++it)
        video->removeTile(it.key());
    video->setFocused(Snowflake::Invalid);
}

void CallVideoWindow::rebuild()
{
    if (!video)
        return;

    const QList<Snowflake> senders = video->senders();

    const QList<Snowflake> shown = tiles.keys();
    for (Snowflake userId : shown) {
        if (senders.contains(userId))
            continue;
        video->removeTile(userId);
        delete tiles.take(userId);
        if (focused == userId)
            focused = Snowflake::Invalid;
    }

    for (Snowflake userId : senders) {
        if (tiles.contains(userId))
            continue;

        auto *surface = new VideoSurface(this);
        surface->setCaption(names ? names(userId) : QString());
        surface->setContextMenuPolicy(Qt::CustomContextMenu);
        tiles.insert(userId, surface);

        connect(surface, &VideoSurface::geometryChanged, this, [this, userId, surface](int pixelCount, bool visible) {
            if (video)
                video->setTile(userId, surface->sink(), pixelCount, visible);
        });
        connect(surface, &VideoSurface::doubleClicked, this, [this, userId] { toggleFocus(userId); });
        connect(surface, &QWidget::customContextMenuRequested, this, [this, userId, surface](const QPoint &pos) { showTileMenu(userId, surface->mapToGlobal(pos)); });
        applyDisabled(userId);
    }

    relayout();
}

void CallVideoWindow::relayout()
{
    for (VideoSurface *surface : std::as_const(tiles))
        grid->removeWidget(surface);
    grid->removeWidget(emptyLabel);

    emptyLabel->setVisible(tiles.isEmpty());
    if (tiles.isEmpty()) {
        grid->addWidget(emptyLabel, 0, 0);
        return;
    }

    // One tile alone when focused; otherwise the squarest grid that holds them all.
    QList<Snowflake> order = tiles.keys();
    std::sort(order.begin(), order.end(), [](Snowflake a, Snowflake b) { return static_cast<quint64>(a) < static_cast<quint64>(b); });

    const bool alone = focused.isValid() && tiles.contains(focused);
    const int columns = alone ? 1 : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(order.size()))));
    int placed = 0;
    for (Snowflake userId : std::as_const(order)) {
        VideoSurface *surface = tiles.value(userId);
        const bool shown = !alone || userId == focused;
        surface->setVisible(shown);
        if (!shown)
            continue;
        grid->addWidget(surface, placed / columns, placed % columns);
        placed++;
    }
}

void CallVideoWindow::toggleFocus(Snowflake userId)
{
    focused = focused == userId ? Snowflake::Invalid : userId;
    if (video)
        video->setFocused(focused);
    relayout();
}

void CallVideoWindow::showTileMenu(Snowflake userId, const QPoint &globalPos)
{
    if (!video)
        return;

    QMenu menu(this);
    QAction *disable = menu.addAction(tr("Disable Video"));
    disable->setCheckable(true);
    disable->setChecked(video->isVideoDisabled(userId));
    if (menu.exec(globalPos) != disable || !video)
        return;

    video->setVideoDisabled(userId, disable->isChecked());
    applyDisabled(userId);
}

void CallVideoWindow::applyDisabled(Snowflake userId)
{
    VideoSurface *surface = tiles.value(userId, nullptr);
    if (!surface || !video)
        return;

    if (video->isVideoDisabled(userId)) {
        surface->clear();
        surface->setMessage(tr("Video Disabled"));
    } else {
        surface->setMessage({});
    }
}

} // namespace UI
} // namespace Acheron
