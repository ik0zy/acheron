#include "StreamPreviews.hpp"

#include <QDateTime>
#include <QPointer>

namespace Acheron {
namespace Core {

StreamPreviews::StreamPreviews(Fetcher fetcher, QObject *parent, Clock clock)
    : QObject(parent), fetcher(std::move(fetcher)), clock(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
}

QUrl StreamPreviews::request(const QString &streamKey)
{
    const qint64 now = clock();
    Entry &entry = entries[streamKey];

    if (!entry.url.isEmpty() && now - entry.fetchedAtMs < FRESH_FOR_MS)
        return entry.url;

    if (entry.fetching || now < entry.retryAtMs)
        return QUrl();

    entry.fetching = true;
    fetcher(streamKey, [self = QPointer<StreamPreviews>(this), streamKey](const Discord::StreamPreviewLookup &lookup) {
        if (self)
            self->onLookupDone(streamKey, lookup);
    });
    return QUrl();
}

void StreamPreviews::onLookupDone(const QString &streamKey, const Discord::StreamPreviewLookup &lookup)
{
    auto it = entries.find(streamKey);
    if (it == entries.end())
        return;

    const qint64 now = clock();
    it->fetching = false;
    it->url = lookup.url;

    if (lookup.url.isEmpty()) {
        it->failures++;
        it->retryAtMs = now + (lookup.retryAfterMs > 0 ? lookup.retryAfterMs : BACKOFF_STEP_MS * it->failures);
    } else {
        it->failures = 0;
        it->retryAtMs = 0;
        it->fetchedAtMs = now;
    }

    emit previewChanged(streamKey, lookup.url);
}

void StreamPreviews::forgetOwner(Snowflake ownerId)
{
    for (auto it = entries.begin(); it != entries.end();) {
        const auto key = Discord::StreamKey::parse(it.key());
        if (key && key->ownerId == ownerId)
            it = entries.erase(it);
        else
            ++it;
    }
}

void StreamPreviews::clear()
{
    entries.clear();
}

} // namespace Core
} // namespace Acheron
