#include "VideoSurface.hpp"

#include <QFontDatabase>
#include <QImage>
#include <QPainter>

#include <mutex>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace UI {

namespace {

constexpr int RESCALE_DELAY_MS = 120;
constexpr int HD_HEIGHT = 720;

// QImage::Format_RGB32 is a 32-bit word per pixel, so its byte order follows the machine's.
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
constexpr AVPixelFormat RGB32_FORMAT = AV_PIX_FMT_ARGB;
#else
constexpr AVPixelFormat RGB32_FORMAT = AV_PIX_FMT_BGRA;
#endif

QSize fitted(QSize content, QSize bounds)
{
    if (content.isEmpty() || bounds.isEmpty())
        return {};
    return content.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

} // namespace

// Converts each decoded frame to RGB at the widget's size. The decoder's thread writes into
// `back` and swaps it with `front`; the widget only ever reads `front`, under the same lock, so
// neither image is shared and none is allocated per frame.
class VideoSurface::Sink : public Core::Media::VideoFrameSink
{
public:
    explicit Sink(VideoSurface *owner)
        : owner(owner), last(av_frame_alloc()) { }

    ~Sink() override
    {
        sws_freeContext(scaler);
        av_frame_free(&last);
    }

    void present(const AVFrame *frame) override
    {
        std::lock_guard lock(mutex);
        if (!owner)
            return;

        av_frame_unref(last);
        av_frame_ref(last, frame);
        convert();

        if (!updatePending) {
            updatePending = true;
            QMetaObject::invokeMethod(owner, [surface = owner] { surface->onFrameArrived(); }, Qt::QueuedConnection);
        }
    }

    void detach()
    {
        std::lock_guard lock(mutex);
        owner = nullptr;
    }

    void setTarget(QSize devicePixels, qreal devicePixelRatio)
    {
        std::lock_guard lock(mutex);
        target = devicePixels;
        pixelRatio = devicePixelRatio;
    }

    // Redraws the last frame at the current size, for when no new frame is coming to do it.
    void rescale()
    {
        std::lock_guard lock(mutex);
        if (last->width > 0)
            convert();
    }

    void forget()
    {
        std::lock_guard lock(mutex);
        av_frame_unref(last);
        front = QImage();
    }

    [[nodiscard]] QSize pictureSize()
    {
        std::lock_guard lock(mutex);
        return fitted(displaySize(), target);
    }

    template <typename Painter>
    void withFront(Painter paint)
    {
        std::lock_guard lock(mutex);
        updatePending = false;
        paint(front);
    }

private:
    [[nodiscard]] QSize displaySize() const
    {
        if (last->width <= 0 || last->height <= 0)
            return {};
        const AVRational aspect = last->sample_aspect_ratio;
        if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
            return QSize(static_cast<int>(static_cast<qint64>(last->width) * aspect.num / aspect.den), last->height);
        return QSize(last->width, last->height);
    }

    void convert()
    {
        const QSize size = fitted(displaySize(), target);
        if (size.isEmpty())
            return;

        const bool newScaler = last->width != sourceSize.width() || last->height != sourceSize.height() || last->format != sourceFormat || size != scaledSize;
        scaler = sws_getCachedContext(scaler, last->width, last->height, static_cast<AVPixelFormat>(last->format), size.width(), size.height(), RGB32_FORMAT, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler)
            return;

        if (newScaler) {
            sourceSize = QSize(last->width, last->height);
            sourceFormat = last->format;
            scaledSize = size;

            // Untagged video is taken as BT.709 at HD sizes and BT.601 below, as players do.
            int matrix = last->colorspace;
            if (matrix == AVCOL_SPC_UNSPECIFIED)
                matrix = last->height >= HD_HEIGHT ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
            const int *coefficients = sws_getCoefficients(matrix);
            const int fullRange = last->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
            sws_setColorspaceDetails(scaler, coefficients, fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        }

        if (back.size() != size)
            back = QImage(size, QImage::Format_RGB32);
        back.setDevicePixelRatio(pixelRatio);

        uint8_t *planes[4] = { back.bits(), nullptr, nullptr, nullptr };
        const int strides[4] = { static_cast<int>(back.bytesPerLine()), 0, 0, 0 };
        sws_scale(scaler, last->data, last->linesize, 0, last->height, planes, strides);
        back.swap(front);
    }

    std::mutex mutex;
    VideoSurface *owner;
    AVFrame *last;
    SwsContext *scaler = nullptr;
    QSize target;
    qreal pixelRatio = 1.0;
    QSize sourceSize;
    QSize scaledSize;
    int sourceFormat = -1;
    QImage front;
    QImage back;
    bool updatePending = false;
};

VideoSurface::VideoSurface(QWidget *parent)
    : QWidget(parent), frameSink(std::make_shared<Sink>(this))
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);

    rescaleTimer.setSingleShot(true);
    rescaleTimer.setInterval(RESCALE_DELAY_MS);
    connect(&rescaleTimer, &QTimer::timeout, this, [this] {
        frameSink->rescale();
        update();
    });
}

VideoSurface::~VideoSurface()
{
    frameSink->detach();
}

std::shared_ptr<Core::Media::VideoFrameSink> VideoSurface::sink() const
{
    return frameSink;
}

void VideoSurface::setMessage(const QString &title, const QString &detail)
{
    if (title == messageTitle && detail == messageDetail)
        return;
    messageTitle = title;
    messageDetail = detail;
    update();
}

void VideoSurface::setStatsText(const QString &text)
{
    if (text == statsText)
        return;
    statsText = text;
    update();
}

void VideoSurface::setCaption(const QString &text)
{
    if (text == captionText)
        return;
    captionText = text;
    update();
}

void VideoSurface::clear()
{
    frameSink->forget();
    update();
}

void VideoSurface::onFrameArrived()
{
    update();
    reportGeometry();
}

void VideoSurface::reportGeometry()
{
    const QSize picture = frameSink->pictureSize();
    // Before the first frame the picture's shape is unknown, so the whole widget stands in.
    const QSize area = picture.isEmpty() ? size() * devicePixelRatioF() : picture;
    emit geometryChanged(area.width() * area.height(), isVisible() && !window()->isMinimized());
}

void VideoSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    frameSink->setTarget(size() * devicePixelRatioF(), devicePixelRatioF());
    rescaleTimer.start();
    reportGeometry();
}

void VideoSurface::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    frameSink->setTarget(size() * devicePixelRatioF(), devicePixelRatioF());
    reportGeometry();
}

void VideoSurface::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    emit geometryChanged(0, false);
}

void VideoSurface::mouseDoubleClickEvent(QMouseEvent *event)
{
    QWidget::mouseDoubleClickEvent(event);
    emit doubleClicked();
}

void VideoSurface::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    frameSink->withFront([&](const QImage &image) {
        if (image.isNull())
            return;
        const qreal ratio = devicePixelRatioF();
        const QSize widgetPixels = size() * ratio;
        if (image.size() == fitted(image.size(), widgetPixels)) {
            // Already at the right size: place it on whole device pixels so it is copied, not resampled.
            const QPoint offset((widgetPixels.width() - image.width()) / 2, (widgetPixels.height() - image.height()) / 2);
            painter.drawImage(QPointF(offset) / ratio, image);
            return;
        }
        // While a resize is in progress the picture is still at its old size.
        const QSizeF shown = QSizeF(image.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        const QRectF where(QPointF((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0), shown);
        painter.drawImage(where, image, QRectF(QPointF(0, 0), QSizeF(image.size())));
    });

    if (!messageTitle.isEmpty()) {
        painter.fillRect(rect(), QColor(0, 0, 0, 160));
        painter.setPen(Qt::white);

        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
        painter.setFont(titleFont);
        const QRect titleArea = rect().adjusted(16, 0, -16, -rect().height() / 2);
        painter.drawText(titleArea, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, messageTitle);

        painter.setFont(font());
        const QRect detailArea = rect().adjusted(16, rect().height() / 2 + 6, -16, 0);
        painter.drawText(detailArea, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, messageDetail);
    }

    if (!captionText.isEmpty()) {
        painter.setFont(font());
        const QRect bounds = painter.boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignBottom, captionText);
        painter.fillRect(bounds.adjusted(-4, -2, 4, 2), QColor(0, 0, 0, 170));
        painter.setPen(Qt::white);
        painter.drawText(bounds, Qt::AlignLeft | Qt::AlignBottom, captionText);
    }

    if (!statsText.isEmpty()) {
        const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        painter.setFont(fixed);
        const QRect bounds = painter.boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop, statsText);
        painter.fillRect(bounds.adjusted(-4, -4, 4, 4), QColor(0, 0, 0, 170));
        painter.setPen(Qt::white);
        painter.drawText(bounds, Qt::AlignLeft | Qt::AlignTop, statsText);
    }
}

} // namespace UI
} // namespace Acheron
