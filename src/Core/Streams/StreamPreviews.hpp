#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <functional>

#include "Core/Snowflake.hpp"
#include "Discord/StreamKey.hpp"

namespace Acheron {
namespace Core {

// The thumbnails stream owners upload, looked up on demand and cached the way the official
// client does: a result is good for two minutes, a failed lookup backs off ten seconds longer
// each time, and a rate limit waits as long as the server asks.
class StreamPreviews : public QObject
{
    Q_OBJECT
public:
    using Fetcher = std::function<void(const QString &streamKey, std::function<void(const Discord::StreamPreviewLookup &)> done)>;
    using Clock = std::function<qint64()>;

    explicit StreamPreviews(Fetcher fetcher, QObject *parent = nullptr, Clock clock = {});

    // The stream's thumbnail when a fresh one is cached. Otherwise empty, with a lookup started
    // unless one is already running or an earlier failure is still backing off; previewChanged
    // reports how it ends.
    QUrl request(const QString &streamKey);

    void forgetOwner(Snowflake ownerId);
    void clear();

    static constexpr qint64 FRESH_FOR_MS = 2 * 60 * 1000;
    static constexpr qint64 BACKOFF_STEP_MS = 10 * 1000;

signals:
    void previewChanged(const QString &streamKey, const QUrl &url);

private:
    struct Entry
    {
        QUrl url;
        qint64 fetchedAtMs = 0;
        qint64 retryAtMs = 0;
        int failures = 0;
        bool fetching = false;
    };

    void onLookupDone(const QString &streamKey, const Discord::StreamPreviewLookup &lookup);

    Fetcher fetcher;
    Clock clock;
    QHash<QString, Entry> entries;
};

// The thumbnail a stream's owner uploads of it, when the official client does: half a second
// after the stream's video starts, five minutes after each upload, and a minute after one that
// failed or came due while the capture was stalled.
class StreamPreviewUploader : public QObject
{
    Q_OBJECT
public:
    // `thumbnail` is a JPEG as a data URL.
    using Upload = std::function<void(const QString &streamKey, const QString &thumbnail, std::function<void(bool uploaded)> done)>;

    explicit StreamPreviewUploader(Upload upload, QObject *parent = nullptr);

    // The setting `video/stream_previews`; on unless the user turns it off.
    [[nodiscard]] static bool enabled();
    // The picture fitted inside 512x288 as a JPEG data URL. Empty when it cannot be made.
    [[nodiscard]] static QString thumbnailOf(const QImage &picture);

    // The stream's video has started, or started again.
    void start(const QString &streamKey);
    void stop();
    void setStalled(bool stalled);
    // Every picture of the stream as it comes. The first one after an upload has come due is
    // the one uploaded.
    void offer(const QImage &picture);

    // How long until the next upload comes due, as it was set; negative when none is waiting.
    [[nodiscard]] int waitMs() const { return due.isActive() ? due.interval() : -1; }

    static constexpr int FIRST_WAIT_MS = 500;
    static constexpr int UPLOADED_WAIT_MS = 5 * 60 * 1000;
    static constexpr int RETRY_WAIT_MS = 60 * 1000;
    static constexpr QSize LARGEST = QSize(512, 288);
    // What a browser's canvas writes a JPEG at when not told, which is how the official client
    // makes its thumbnails.
    static constexpr int JPEG_QUALITY = 92;

private slots:
    void onDue();

private:
    void onUploadDone(quint64 startedBy, bool uploaded);

    Upload upload;
    QString streamKey;
    QTimer due;
    bool stalled = false;
    bool pictureWanted = false;
    // Counts the starts and stops, so that an upload's answer is told from one for a stream
    // that has since stopped.
    quint64 run = 0;
};

} // namespace Core
} // namespace Acheron
