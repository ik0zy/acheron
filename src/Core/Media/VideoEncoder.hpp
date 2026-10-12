#pragma once

#include <QList>
#include <QString>

#include <cstdint>
#include <vector>

#include "Discord/Voice/VideoReceiver.hpp"

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace Acheron {
namespace Core {
namespace Media {

struct VideoEncoderCandidate;

// The codecs this machine can encode, of the two the RTP sender packetizes: H264 and VP8, in
// that order. A codec counts when one of its encoders opens and encodes a few pictures, which
// is tried once and can take a moment. Any thread may ask.
[[nodiscard]] QList<Discord::Voice::VideoCodec> encodableVideoCodecs();

// Every encoder of a codec this build could use, best first, whether or not it works on this
// machine: the names VideoEncoder::Settings::encoderName takes.
[[nodiscard]] QList<QString> videoEncoderNames(Discord::Voice::VideoCodec codec);
// Whether the named encoder opens on this machine and hands every frame back with the picture
// it was made from. Tried once, like the codecs.
[[nodiscard]] bool videoEncoderWorks(const QString &name);

// Encodes live video with as little delay as the encoder allows: a picture goes in and its
// encoded frame comes out of the same call. It takes the best encoder that works, the graphics
// card's before a software one, and moves on to the next when one that worked stops working.
//
// Encoded frames are whole access units: Annex B for H264 with SPS and PPS in front of every
// key frame, a bare frame for VP8. Not thread safe; it belongs to the thread that feeds it.
class VideoEncoder
{
public:
    struct Settings
    {
        Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
        int framerate = 30;
        // Bits per second.
        int bitrate = 0;
        // Zero makes key frames only when asked for.
        int keyframeIntervalMs = 0;
        // One of videoEncoderNames(), to use that encoder or none. Empty takes the best that works.
        QString encoderName;
    };

    enum class Outcome {
        Encoded,
        // The picture was on the graphics card and the encoder cannot take it from there. Nothing
        // is held against the encoder: the same picture in system memory is to be sent instead.
        CardFramesRefused,
        // No encoder is left that works.
        Failed,
    };

    struct Stats
    {
        // The encoder in use, with the hardware device in brackets when pictures reach it on the card.
        QString encoderName;
        bool hardware = false;
        int width = 0;
        int height = 0;
        quint64 encoded = 0;
        quint64 opened = 0;
        double averageEncodeMs = 0;
    };

    explicit VideoEncoder(const Settings &settings);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    [[nodiscard]] bool hasEncoder() const { return candidate != nullptr; }
    // Whether the encoder the next picture goes to takes NV12 pictures as Direct3D 11 frames,
    // and whether it takes the screen's own BGRA ones as well, converting them itself to the
    // colours the frame is marked with.
    [[nodiscard]] bool takesCardFrames() const;
    [[nodiscard]] bool takesRgbCardFrames() const;
    // The format (an AVPixelFormat) pictures in system memory must have.
    [[nodiscard]] int softwareFormat() const;

    // The next picture becomes a key frame.
    void requestKeyframe();
    // Followed while running by the encoders that can; the others start over with the next
    // picture, which is then a key frame.
    void setBitrate(int bitrate);

    // Encodes one picture, whose size and format may differ from the last one's. `timestamp` is
    // in 90 kHz units, must grow, and comes back as the frame's RTP timestamp. The frame's own
    // timestamp and picture type are overwritten. Appends to `out`: one frame, or none when the
    // encoder's rate control dropped the picture.
    Outcome encode(AVFrame *frame, int64_t timestamp, std::vector<Discord::Voice::EncodedVideoFrame> &out);

    [[nodiscard]] const Stats &stats() const { return counters; }

    // An encoder that fails before it has made this many frames is given up on.
    static constexpr int TRIAL_FRAMES = 30;
    // How long a picture's frame is waited for before the encoder is taken to be running late.
    static constexpr int OUTPUT_WAIT_MS = 250;

private:
    bool encodeWithOpenEncoder(AVFrame *frame, int64_t timestamp, std::vector<Discord::Voice::EncodedVideoFrame> &out);
    bool open(const AVFrame *frame);
    AVCodecContext *configured(const AVFrame *frame, int h264Profile) const;
    bool prepareUpload(const AVFrame *frame);
    AVFrame *uploadedCopy(const AVFrame *frame);
    void close();
    void moveToNextEncoder();
    bool collect(std::vector<Discord::Voice::EncodedVideoFrame> &out);
    [[nodiscard]] bool finishH264(Discord::Voice::EncodedVideoFrame &frame);
    void rememberParameterSets(const uint8_t *data, size_t size);
    [[nodiscard]] int bufferBits() const;

    Settings settings;
    const VideoEncoderCandidate *candidate = nullptr;
    AVCodecContext *context = nullptr;
    AVBufferRef *uploadFrames = nullptr;
    AVFrame *uploaded = nullptr;
    AVPacket *packet = nullptr;

    bool cardFramesRefused = false;
    bool keyframeWanted = false;
    bool reopenWanted = false;
    int awaited = 0;
    quint64 framesSinceOpen = 0;
    int64_t lastKeyframeAt = 0;
    // The parameter sets last seen, for a key frame that comes without them.
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    Stats counters;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
