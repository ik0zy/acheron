#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>
#include <mutex>
#include <unordered_map>

#include "Core/Media/VideoDecoder.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Video/TileVisibility.hpp"
#include "Discord/Voice/SinkWants.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

// The cameras of the voice call this account is in. It knows who is sending video, decodes the
// ones a tile is shown for, and tells the server how much of each is wanted.
class CallVideo : public QObject
{
    Q_OBJECT
public:
    explicit CallVideo(QObject *parent = nullptr);
    ~CallVideo() override;

    // Follows one voice connection. Call before the client starts, and detach() once it has
    // stopped.
    void attach(Discord::Voice::VoiceClient *client);
    void detach();

    [[nodiscard]] QList<Snowflake> senders() const;

    // A tile for a sender's video: where its frames go, the size it is drawn at in device
    // pixels, and whether it is on screen.
    void setTile(Snowflake userId, std::shared_ptr<Media::VideoFrameSink> sink, int pixelCount, bool visible);
    void removeTile(Snowflake userId);

    // The participant shown large, who is asked for full quality.
    void setFocused(Snowflake userId);
    void setOneToOne(bool oneToOne);

    // "Disable Video" for one person, remembered across sessions.
    [[nodiscard]] bool isVideoDisabled(Snowflake userId) const;
    void setVideoDisabled(Snowflake userId, bool disabled);

    // Empty for a sender that is not being decoded.
    [[nodiscard]] Media::VideoDecoder::Stats decoderStats(Snowflake userId) const;

    // At most this many cameras are decoded at once; the rest keep their avatars.
    [[nodiscard]] static int maxDecoders();
    static void setMaxDecoders(int decoders);

    static constexpr int REPLAN_DELAY_MS = 100;
    static constexpr int DEFAULT_MAX_DECODERS = 9;

signals:
    void sendersChanged();
    void videoDisabledChanged(Snowflake userId);

private slots:
    void onLayersChanged(Snowflake userId, const QList<Discord::Voice::VideoLayer> &layers);
    void replan();

private:
    struct Tile
    {
        std::shared_ptr<Media::VideoFrameSink> sink;
        int pixels = 0;
    };

    void scheduleReplan();

    Discord::Voice::VoiceClient *client = nullptr;
    QHash<Snowflake, QList<Discord::Voice::VideoLayer>> layersByUser;
    QHash<Snowflake, Tile> tiles;
    TileVisibility visibility;
    QSet<Snowflake> disabledUsers;
    Snowflake focusedUser;
    bool oneToOne = false;

    QElapsedTimer clock;
    QTimer replanTimer;
    QTimer expiryTimer;

    // Read on the connection's thread for every frame.
    mutable std::mutex decoderMutex;
    std::unordered_map<quint64, std::shared_ptr<Media::VideoDecoder>> decoders;
};

} // namespace Core
} // namespace Acheron
