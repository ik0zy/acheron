#include "ScreenSharePicker.hpp"

#include "Core/Streams/StreamBroadcaster.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

using Core::Video::SendQuality;

namespace {

const char *const HEIGHT_KEY = "video/share_height";
const char *const FRAMERATE_KEY = "video/share_framerate";

constexpr int VISIBLE_SOURCE_ROWS = 2;
constexpr int SOURCE_LABEL_HEIGHT = 36;

void setItemEnabled(QComboBox *combo, int index, bool enabled)
{
    if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
        model->item(index)->setEnabled(enabled);
}

} // namespace

ScreenSharePicker::ScreenSharePicker(const Core::Video::SharerTier &tier, QWidget *parent)
    : BasePopup(parent), tier(tier), quality(rememberedQuality(tier))
{
    auto *layout = new QVBoxLayout(getContainer());
    layout->setSpacing(10);
    layout->setContentsMargins(16, 16, 16, 16);

    auto *titleLabel = new QLabel(tr("Share Your Screen"), getContainer());
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    sourceList = new QListWidget(getContainer());
    sourceList->setViewMode(QListView::IconMode);
    sourceList->setFlow(QListView::LeftToRight);
    sourceList->setWrapping(true);
    sourceList->setResizeMode(QListView::Adjust);
    sourceList->setMovement(QListView::Static);
    sourceList->setSelectionMode(QAbstractItemView::SingleSelection);
    sourceList->setIconSize(THUMBNAIL_SIZE);
    sourceList->setSpacing(6);
    sourceList->setMinimumWidth(THUMBNAIL_SIZE.width() * 2 + 48);
    sourceList->setFixedHeight((THUMBNAIL_SIZE.height() + SOURCE_LABEL_HEIGHT) * VISIBLE_SOURCE_ROWS);
    layout->addWidget(sourceList);
    connect(sourceList, &QListWidget::itemSelectionChanged, this, &ScreenSharePicker::refreshShareButton);
    connect(sourceList, &QListWidget::itemActivated, this, &ScreenSharePicker::onShareClicked);

    auto *qualityRow = new QHBoxLayout;
    qualityRow->addWidget(new QLabel(tr("Resolution"), getContainer()));
    heightCombo = new QComboBox(getContainer());
    for (const int height : Core::Video::SEND_HEIGHTS)
        heightCombo->addItem(heightLabel(height), height);
    qualityRow->addWidget(heightCombo);
    qualityRow->addSpacing(12);
    qualityRow->addWidget(new QLabel(tr("Frame Rate"), getContainer()));
    framerateCombo = new QComboBox(getContainer());
    for (const int framerate : Core::Video::SEND_FRAMERATES)
        framerateCombo->addItem(framerateLabel(framerate), framerate);
    qualityRow->addWidget(framerateCombo);
    qualityRow->addStretch(1);
    layout->addLayout(qualityRow);
    connect(heightCombo, QOverload<int>::of(&QComboBox::activated), this, &ScreenSharePicker::onHeightChosen);
    connect(framerateCombo, QOverload<int>::of(&QComboBox::activated), this, &ScreenSharePicker::onFramerateChosen);

    noteLabel = new QLabel(getContainer());
    noteLabel->setWordWrap(true);
    layout->addWidget(noteLabel);

    auto *buttons = new QDialogButtonBox(getContainer());
    shareButton = buttons->addButton(tr("Share"), QDialogButtonBox::AcceptRole);
    shareButton->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ScreenSharePicker::onShareClicked);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    addSources();
    showQuality();
    refreshShareButton();
    Core::StreamBroadcaster::whenEncodersKnown(this, [this](const QList<Discord::Voice::VideoCodec> &codecs) { onEncodersKnown(codecs); });
}

SendQuality ScreenSharePicker::rememberedQuality(const Core::Video::SharerTier &tier)
{
    const SendQuality standard;
    QSettings settings;
    const SendQuality remembered{ settings.value(HEIGHT_KEY, standard.height).toInt(), settings.value(FRAMERATE_KEY, standard.framerate).toInt() };
    return Core::Video::usableQuality(remembered, tier);
}

void ScreenSharePicker::rememberQuality(const SendQuality &quality)
{
    QSettings settings;
    settings.setValue(HEIGHT_KEY, quality.height);
    settings.setValue(FRAMERATE_KEY, quality.framerate);
}

QString ScreenSharePicker::heightLabel(int height)
{
    return height == SendQuality::SOURCE_HEIGHT ? tr("Source") : tr("%1p").arg(height);
}

QString ScreenSharePicker::framerateLabel(int framerate)
{
    return tr("%1 fps").arg(framerate);
}

void ScreenSharePicker::addSources()
{
    sources = Core::Video::captureSources();

    QPixmap blank(THUMBNAIL_SIZE);
    blank.fill(Qt::black);
    for (int i = 0; i < sources.size(); i++) {
        const Core::Video::CaptureSource &source = sources[i];
        const QString name = source.name.isEmpty() ? tr("Screen %1").arg(i + 1) : source.name;
        auto *item = new QListWidgetItem(QIcon(blank), tr("%1 (%2x%3)").arg(name).arg(source.geometry.width()).arg(source.geometry.height()), sourceList);
        item->setData(Qt::UserRole, source.id);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    }
    if (!sources.isEmpty())
        sourceList->setCurrentRow(0);

    // One still for each turn of the event loop, so that the picker is up before the first.
    QTimer::singleShot(0, this, &ScreenSharePicker::loadNextThumbnail);
}

void ScreenSharePicker::loadNextThumbnail()
{
    if (thumbnailsLoaded >= sources.size())
        return;

    const int index = thumbnailsLoaded++;
    const QImage still = Core::Video::captureThumbnail(sources[index], THUMBNAIL_SIZE * devicePixelRatioF());
    if (!still.isNull()) {
        QPixmap thumbnail(THUMBNAIL_SIZE * devicePixelRatioF());
        thumbnail.fill(Qt::black);
        const QImage fitted = still.scaled(thumbnail.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter painter(&thumbnail);
        painter.drawImage((thumbnail.width() - fitted.width()) / 2, (thumbnail.height() - fitted.height()) / 2, fitted);
        painter.end();
        thumbnail.setDevicePixelRatio(devicePixelRatioF());
        sourceList->item(index)->setIcon(QIcon(thumbnail));
    }
    QTimer::singleShot(0, this, &ScreenSharePicker::loadNextThumbnail);
}

void ScreenSharePicker::showQuality()
{
    for (int i = 0; i < heightCombo->count(); i++)
        setItemEnabled(heightCombo, i, Core::Video::mayUseQuality(Core::Video::atHeight(quality, heightCombo->itemData(i).toInt(), tier), tier));
    for (int i = 0; i < framerateCombo->count(); i++)
        setItemEnabled(framerateCombo, i, Core::Video::mayUseQuality({ quality.height, framerateCombo->itemData(i).toInt() }, tier));

    heightCombo->setCurrentIndex(heightCombo->findData(quality.height));
    framerateCombo->setCurrentIndex(framerateCombo->findData(quality.framerate));
}

void ScreenSharePicker::onHeightChosen(int index)
{
    quality = Core::Video::atHeight(quality, heightCombo->itemData(index).toInt(), tier);
    showQuality();
}

void ScreenSharePicker::onFramerateChosen(int index)
{
    quality.framerate = framerateCombo->itemData(index).toInt();
}

void ScreenSharePicker::onEncodersKnown(const QList<Discord::Voice::VideoCodec> &codecs)
{
    encodersKnown = true;
    canEncode = !codecs.isEmpty();
    refreshShareButton();
}

void ScreenSharePicker::refreshShareButton()
{
    if (sources.isEmpty())
        noteLabel->setText(tr("No screen can be captured on this machine."));
    else if (!encodersKnown)
        noteLabel->setText(tr("Checking this machine's video encoders..."));
    else if (!canEncode)
        noteLabel->setText(tr("This machine has no video encoder that Discord accepts, so it cannot share a screen."));
    else
        noteLabel->clear();
    noteLabel->setVisible(!noteLabel->text().isEmpty());

    shareButton->setEnabled(canEncode && sourceList->currentItem() != nullptr);
}

void ScreenSharePicker::onShareClicked()
{
    QListWidgetItem *chosen = sourceList->currentItem();
    if (!canEncode || !chosen)
        return;

    rememberQuality(quality);
    emit shareRequested(chosen->data(Qt::UserRole).toString(), quality);
    accept();
}

} // namespace UI
} // namespace Acheron
