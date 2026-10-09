#pragma once

#include <QHash>
#include <QObject>
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

} // namespace Core
} // namespace Acheron
