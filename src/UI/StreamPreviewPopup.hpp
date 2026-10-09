#pragma once

#include <QFrame>
#include <QPointer>
#include <QUrl>

#include "Core/Snowflake.hpp"

class QLabel;

namespace Acheron {
namespace Core {
class ImageManager;
class StreamPreviews;
} // namespace Core

namespace UI {

// The thumbnail shown beside a live user's row in the channel list while it is hovered.
class StreamPreviewPopup : public QFrame
{
    Q_OBJECT
public:
    explicit StreamPreviewPopup(Core::ImageManager *imageManager, QWidget *parent = nullptr);

    // `previews` is null when the account may not look at the stream.
    void showFor(Core::StreamPreviews *previews, Core::Snowflake accountId, const QString &streamKey, const QRect &rowOnScreen);
    void dismiss();

private:
    void showThumbnail(const QUrl &url);
    void showMessage(const QString &message);
    void placeBeside(const QRect &rowOnScreen);

    Core::ImageManager *imageManager;
    QPointer<Core::StreamPreviews> previews;
    QMetaObject::Connection previewChangedConnection;
    Core::Snowflake accountId;
    QString streamKey;
    QUrl thumbnailUrl;

    QLabel *thumbnail;

    static constexpr QSize THUMBNAIL_SIZE = QSize(240, 135);
};

} // namespace UI
} // namespace Acheron
