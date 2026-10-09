#pragma once

#include <QList>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "Discord/Voice/VideoReceiver.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;

namespace Acheron {
namespace Core {
namespace Media {

// The video codecs the loaded ffmpeg can decode in software, in the order the official client
// prefers them: AV1, H265, H264, VP8. Probed once.
[[nodiscard]] QList<Discord::Voice::VideoCodec> decodableVideoCodecs();

// The same as a SELECT_PROTOCOL codec list: payload types are positional, and every entry is
// decode only.
[[nodiscard]] QList<Discord::Voice::Codec> offeredVideoCodecs();

// Where decoded frames go. present() runs on the decoder's thread and must not keep the frame
// past the call without taking its own reference.
class VideoFrameSink
{
public:
    virtual ~VideoFrameSink() = default;
    virtual void present(const AVFrame *frame) = 0;
};

// Decodes one remote video on its own thread. Frames may be pushed from any thread; when the
// decoder falls behind or meets a frame it cannot use, it drops to the next key frame and asks
// for one.
class VideoDecoder
{
public:
    struct Stats
    {
        QString decoderName;
        int width = 0;
        int height = 0;
        quint64 decoded = 0;
        quint64 dropped = 0;
        quint64 errors = 0;
        double averageDecodeMs = 0;
    };

    VideoDecoder();
    ~VideoDecoder();

    void setSink(std::shared_ptr<VideoFrameSink> sink);
    // Called on the decoder's thread.
    void setKeyframeRequester(std::function<void()> requester);

    void push(Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame);

    [[nodiscard]] Stats stats() const;

    static constexpr int MAX_QUEUED_FRAMES = 90;

private:
    struct Queued
    {
        Discord::Voice::VideoCodec codec;
        Discord::Voice::EncodedVideoFrame frame;
    };

    void run();
    bool open(Discord::Voice::VideoCodec codec);
    void close();
    void decode(Queued &queued);
    void fail();

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Queued> queue;
    bool stopping = false;
    std::shared_ptr<VideoFrameSink> frameSink;
    std::function<void()> keyframeRequester;
    Stats counters;

    // Decoder thread only.
    AVCodecContext *context = nullptr;
    AVFrame *decoded = nullptr;
    Discord::Voice::VideoCodec openCodec = Discord::Voice::VideoCodec::Unknown;
    bool awaitingKeyframe = true;

    std::thread thread;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
