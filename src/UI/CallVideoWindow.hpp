#pragma once

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <functional>

#include "Core/Snowflake.hpp"
#include "Core/Video/CallVideo.hpp"

class QGridLayout;
class QLabel;

namespace Acheron {
namespace UI {

class VideoSurface;

// The cameras of the call this account is in, one tile each. Double-clicking a tile shows it
// alone, and again brings the grid back.
class CallVideoWindow : public QWidget
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;

    CallVideoWindow(Core::CallVideo *video, NameResolver names, const QString &title, QWidget *parent = nullptr);
    ~CallVideoWindow() override;

private:
    void rebuild();
    void relayout();
    void toggleFocus(Core::Snowflake userId);
    void showTileMenu(Core::Snowflake userId, const QPoint &globalPos);
    void applyDisabled(Core::Snowflake userId);

    QPointer<Core::CallVideo> video;
    NameResolver names;

    QHash<Core::Snowflake, VideoSurface *> tiles;
    Core::Snowflake focused;
    QGridLayout *grid;
    QLabel *emptyLabel;
};

} // namespace UI
} // namespace Acheron
