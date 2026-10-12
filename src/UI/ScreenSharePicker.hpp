#pragma once

#include <QList>

#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/SendQuality.hpp"
#include "UI/Dialogs/BasePopup.hpp"

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace Acheron {
namespace UI {

// Picks the screen to share and what to send it at. The qualities the account may not use are
// greyed, and the choice is remembered for the next time.
class ScreenSharePicker : public BasePopup
{
    Q_OBJECT
public:
    explicit ScreenSharePicker(const Core::Video::SharerTier &tier, QWidget *parent = nullptr);

    // The quality last chosen, where `tier` still allows it.
    [[nodiscard]] static Core::Video::SendQuality rememberedQuality(const Core::Video::SharerTier &tier);
    static void rememberQuality(const Core::Video::SendQuality &quality);

    [[nodiscard]] static QString heightLabel(int height);
    [[nodiscard]] static QString framerateLabel(int framerate);

    static constexpr QSize THUMBNAIL_SIZE = QSize(224, 126);

signals:
    void shareRequested(const QString &sourceId, const Acheron::Core::Video::SendQuality &quality);

private slots:
    void onHeightChosen(int index);
    void onFramerateChosen(int index);
    void onShareClicked();
    void onEncodersKnown(const QList<Acheron::Discord::Voice::VideoCodec> &codecs);

private:
    void addSources();
    void loadNextThumbnail();
    void showQuality();
    void refreshShareButton();

    Core::Video::SharerTier tier;
    Core::Video::SendQuality quality;
    QList<Core::Video::CaptureSource> sources;
    int thumbnailsLoaded = 0;
    bool encodersKnown = false;
    bool canEncode = false;

    QListWidget *sourceList;
    QComboBox *heightCombo;
    QComboBox *framerateCombo;
    QLabel *noteLabel;
    QPushButton *shareButton;
};

} // namespace UI
} // namespace Acheron
