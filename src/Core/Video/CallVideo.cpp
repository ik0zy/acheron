#include "CallVideo.hpp"

#include <QSettings>

#include <algorithm>

namespace Acheron {
namespace Core {

using Discord::Voice::VoiceClient;

namespace {

const char *const DISABLED_USERS_KEY = "video/disabled_users";
const char *const MAX_DECODERS_KEY = "video/max_decoders";

} // namespace

CallVideo::CallVideo(QObject *parent)
    : QObject(parent)
{
    clock.start();

    const QStringList stored = QSettings().value(DISABLED_USERS_KEY).toStringList();
    for (const QString &id : stored)
        disabledUsers.insert(Snowflake(id.toULongLong()));

    replanTimer.setSingleShot(true);
    replanTimer.setInterval(REPLAN_DELAY_MS);
    connect(&replanTimer, &QTimer::timeout, this, &CallVideo::replan);

    // Hidden tiles lose their video as time passes, with nothing else to prompt a new plan.
    expiryTimer.setInterval(1000);
    connect(&expiryTimer, &QTimer::timeout, this, &CallVideo::replan);
}

CallVideo::~CallVideo()
{
    detach();
}

int CallVideo::maxDecoders()
{
    return std::max(1, QSettings().value(MAX_DECODERS_KEY, DEFAULT_MAX_DECODERS).toInt());
}

void CallVideo::attach(VoiceClient *voiceClient)
{
    detach();
    client = voiceClient;

    connect(client, &VoiceClient::remoteVideoLayersChanged, this, &CallVideo::onLayersChanged);
    connect(client, &VoiceClient::connected, this, &CallVideo::scheduleReplan);
    client->setVideoFrameHandler([this](Snowflake userId, Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame) {
        std::shared_ptr<Media::VideoDecoder> decoder;
        {
            std::lock_guard lock(decoderMutex);
            auto it = decoders.find(static_cast<quint64>(userId));
            if (it != decoders.end())
                decoder = it->second;
        }
        if (decoder)
            decoder->push(codec, std::move(frame));
    });
}

void CallVideo::detach()
{
    if (client)
        client->disconnect(this);
    client = nullptr;
    replanTimer.stop();
    expiryTimer.stop();

    std::unordered_map<quint64, std::shared_ptr<Media::VideoDecoder>> stopping;
    {
        std::lock_guard lock(decoderMutex);
        stopping.swap(decoders);
    }
    stopping.clear();

    if (!layersByUser.isEmpty()) {
        layersByUser.clear();
        emit sendersChanged();
    }
}

QList<Snowflake> CallVideo::senders() const
{
    QList<Snowflake> users = layersByUser.keys();
    std::sort(users.begin(), users.end(), [](Snowflake a, Snowflake b) { return static_cast<quint64>(a) < static_cast<quint64>(b); });
    return users;
}

void CallVideo::onLayersChanged(Snowflake userId, const QList<Discord::Voice::VideoLayer> &layers)
{
    const bool wasSending = layersByUser.contains(userId);
    if (layers.isEmpty())
        layersByUser.remove(userId);
    else
        layersByUser.insert(userId, layers);

    scheduleReplan();
    if (wasSending != !layers.isEmpty())
        emit sendersChanged();
}

void CallVideo::setTile(Snowflake userId, std::shared_ptr<Media::VideoFrameSink> sink, int pixelCount, bool visible)
{
    Tile &tile = tiles[userId];
    tile.sink = std::move(sink);
    tile.pixels = pixelCount;
    visibility.setVisible(userId, visible, clock.elapsed());

    {
        std::lock_guard lock(decoderMutex);
        auto it = decoders.find(static_cast<quint64>(userId));
        if (it != decoders.end())
            it->second->setSink(tile.sink);
    }
    scheduleReplan();
}

void CallVideo::removeTile(Snowflake userId)
{
    if (!tiles.remove(userId))
        return;
    visibility.remove(userId);
    scheduleReplan();
}

void CallVideo::setFocused(Snowflake userId)
{
    if (focusedUser == userId)
        return;
    focusedUser = userId;
    scheduleReplan();
}

void CallVideo::setOneToOne(bool value)
{
    if (oneToOne == value)
        return;
    oneToOne = value;
    scheduleReplan();
}

bool CallVideo::isVideoDisabled(Snowflake userId) const
{
    return disabledUsers.contains(userId);
}

void CallVideo::setVideoDisabled(Snowflake userId, bool disabled)
{
    if (disabled == disabledUsers.contains(userId))
        return;
    if (disabled)
        disabledUsers.insert(userId);
    else
        disabledUsers.remove(userId);

    QStringList stored;
    for (Snowflake id : std::as_const(disabledUsers))
        stored.append(QString::number(static_cast<quint64>(id)));
    QSettings().setValue(DISABLED_USERS_KEY, stored);

    scheduleReplan();
}

Media::VideoDecoder::Stats CallVideo::decoderStats(Snowflake userId) const
{
    std::lock_guard lock(decoderMutex);
    auto it = decoders.find(static_cast<quint64>(userId));
    return it != decoders.end() ? it->second->stats() : Media::VideoDecoder::Stats();
}

void CallVideo::scheduleReplan()
{
    if (client && !replanTimer.isActive())
        replanTimer.start();
}

void CallVideo::replan()
{
    if (!client)
        return;

    const qint64 now = clock.elapsed();
    const int decoderLimit = maxDecoders();

    Discord::Voice::SinkWantsInput input;
    input.focusedUser = focusedUser;
    input.oneToOne = oneToOne;

    // The focused sender is considered first so that the decoder limit never leaves it out.
    QList<Snowflake> users = senders();
    const int focusedAt = users.indexOf(focusedUser);
    if (focusedAt > 0)
        users.move(focusedAt, 0);

    int wantedCount = 0;
    for (Snowflake userId : std::as_const(users)) {
        Discord::Voice::VideoSender sender;
        sender.userId = userId;
        sender.layers = layersByUser.value(userId);
        sender.tilePixels = tiles.value(userId).pixels;
        sender.wanted = tiles.contains(userId) && !disabledUsers.contains(userId) && visibility.wanted(userId, now) && wantedCount < decoderLimit;
        if (sender.wanted)
            wantedCount++;
        input.senders.append(sender);
    }

    const Discord::Voice::SinkWantsPlan plan = Discord::Voice::planSinkWants(input);

    // A decoder for every sender being received, and for nobody else.
    std::unordered_map<quint64, std::shared_ptr<Media::VideoDecoder>> retired;
    {
        std::lock_guard lock(decoderMutex);
        for (auto it = decoders.begin(); it != decoders.end();) {
            if (plan.deliver.contains(Snowflake(it->first))) {
                ++it;
                continue;
            }
            retired.insert(*it);
            it = decoders.erase(it);
        }
        for (auto it = plan.deliver.constBegin(); it != plan.deliver.constEnd(); ++it) {
            const quint64 key = static_cast<quint64>(it.key());
            if (decoders.count(key))
                continue;
            // A camera is at most 720p, which one thread decodes with room to spare.
            auto decoder = std::make_shared<Media::VideoDecoder>(Media::hardwareDecodingEnabled(), 1);
            decoder->setSink(tiles.value(it.key()).sink);
            decoder->setKeyframeRequester([target = client] { QMetaObject::invokeMethod(target, &VoiceClient::requestVideoKeyframe); });
            decoder->setCodecFailureHandler([target = client](Discord::Voice::VideoCodec codec) { QMetaObject::invokeMethod(target, [target, codec] { target->withdrawVideoCodec(codec); }); });
            decoders.emplace(key, std::move(decoder));
        }
    }
    retired.clear();

    QSet<quint32> deliver;
    for (quint32 ssrc : plan.deliver)
        deliver.insert(ssrc);
    QMetaObject::invokeMethod(client, [target = client, wants = plan.wants, deliver] { target->setVideoPlan(wants, deliver); });

    if (visibility.hasPendingExpiry(now))
        expiryTimer.start();
    else
        expiryTimer.stop();
}

} // namespace Core
} // namespace Acheron
