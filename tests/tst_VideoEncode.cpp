// Real encodes. Synthetic pictures go through every encoder this machine has and back through
// the decoder, and the pipeline tests share the actual screen and windows of the test's own
// that it puts on it. What the screen showed is only ever counted and measured here: never
// printed, never written anywhere.

#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/CardScaler.hpp"
#include "Core/Video/ScreenCapture.hpp"

#include <QElapsedTimer>
#include <QMutex>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#ifdef Q_OS_WIN
#include <windows.h>

#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

using namespace Acheron;
using namespace Acheron::Discord::Voice;
using Core::Media::VideoEncoder;

namespace {

constexpr int WIDTH = 640;
constexpr int HEIGHT = 360;
constexpr int FRAMERATE = 30;
constexpr int TICKS_PER_FRAME = 90000 / FRAMERATE;
constexpr int BITRATE = 1'500'000;

// In the order the voice server prefers them.
constexpr std::array CODECS = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

// The colour of the patch in the middle of a synthetic RGB picture.
constexpr int PATCH_RED = 40;
constexpr int PATCH_GREEN = 200;
constexpr int PATCH_BLUE = 60;

struct EncoderCase
{
    VideoCodec codec = VideoCodec::Unknown;
    // Empty for whichever encoder the probe settled on.
    QString encoderName;
    // For the tests that take one: the format of the pictures, or the tallest picture to send.
    int detail = 0;
};

struct Yuv
{
    double y = 0;
    double u = 0;
    double v = 0;
};

// What a colour comes to in video-range YUV under either set of coefficients.
Yuv inYuv(int red, int green, int blue, bool bt709)
{
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double luma = kr * red + (1 - kr - kb) * green + kb * blue;
    return { 16 + 219 * luma / 255, 128 + 224 * (blue - luma) / (2 * (1 - kb)) / 255, 128 + 224 * (red - luma) / (2 * (1 - kr)) / 255 };
}

Yuv patchInYuv(bool bt709)
{
    return inYuv(PATCH_RED, PATCH_GREEN, PATCH_BLUE, bt709);
}

double distance(const Yuv &a, const Yuv &b)
{
    return std::sqrt((a.y - b.y) * (a.y - b.y) + (a.u - b.u) * (a.u - b.u) + (a.v - b.v) * (a.v - b.v));
}

// Counts decoded pictures, and keeps the colours in the middle and at the top of the last one.
// Each picture is shown to `inspect` with its place among them, on the decoder's thread.
class CountingSink : public Core::Media::VideoFrameSink
{
public:
    static constexpr int TOP_ROW = 4;

    void present(const AVFrame *frame) override
    {
        if (inspect)
            inspect(frame, frames);
        width = frame->width;
        height = frame->height;
        format = frame->format;
        colorspace = frame->colorspace;
        range = frame->color_range;
        shapeWidth = frame->sample_aspect_ratio.num;
        shapeHeight = frame->sample_aspect_ratio.den;
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            const int x = frame->width / 2;
            const int y = frame->height / 2;
            middleY = frame->data[0][y * frame->linesize[0] + x];
            middleU = frame->data[1][(y / 2) * frame->linesize[1] + x / 2];
            middleV = frame->data[2][(y / 2) * frame->linesize[2] + x / 2];
            topY = frame->data[0][TOP_ROW * frame->linesize[0] + x];
            topU = frame->data[1][(TOP_ROW / 2) * frame->linesize[1] + x / 2];
            topV = frame->data[2][(TOP_ROW / 2) * frame->linesize[2] + x / 2];
        }
        frames++;
    }

    std::function<void(const AVFrame *, int)> inspect;
    std::atomic<int> frames{ 0 };
    std::atomic<int> width{ 0 };
    std::atomic<int> height{ 0 };
    std::atomic<int> format{ AV_PIX_FMT_NONE };
    std::atomic<int> colorspace{ AVCOL_SPC_UNSPECIFIED };
    std::atomic<int> range{ AVCOL_RANGE_UNSPECIFIED };
    // The frame's sample aspect ratio, which is how ffmpeg passes on the size AV1 says a
    // picture is to be shown at when that is not the size it was coded at.
    std::atomic<int> shapeWidth{ 0 };
    std::atomic<int> shapeHeight{ 1 };
    std::atomic<int> middleY{ 0 };
    std::atomic<int> middleU{ 0 };
    std::atomic<int> middleV{ 0 };
    std::atomic<int> topY{ 0 };
    std::atomic<int> topU{ 0 };
    std::atomic<int> topV{ 0 };
};

// A gradient that drifts, a patch of fine texture that slides across it, and a band of noise
// that is new in every picture: enough that an encoder has to spend what it is given.
void paintYuv(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            uint8_t value = static_cast<uint8_t>((x + y) / 4 + index * 3);
            if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
            if (x < width / 4 && y < height / 6) {
                noise = noise * 1664525u + 1013904223u;
                value = static_cast<uint8_t>(96 + (noise >> 26));
            }
            row[x] = value;
        }
    }

    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width / 2; x++) {
            const auto u = static_cast<uint8_t>(128 + y / 4 + index * 2);
            const auto v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            if (interleaved) {
                picture->data[1][y * picture->linesize[1] + 2 * x] = u;
                picture->data[1][y * picture->linesize[1] + 2 * x + 1] = v;
            } else {
                picture->data[1][y * picture->linesize[1] + x] = u;
                picture->data[2][y * picture->linesize[2] + x] = v;
            }
        }
    }
}

// Drifting gradients around a patch of one colour, which is what the colour checks look at.
void paintBgra(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            const bool inPatch = std::abs(x - width / 2) < width / 8 && std::abs(y - height / 2) < height / 8;
            row[4 * x] = inPatch ? PATCH_BLUE : static_cast<uint8_t>(x / 2 + index * 3);
            row[4 * x + 1] = inPatch ? PATCH_GREEN : static_cast<uint8_t>(y / 2 + index * 2);
            row[4 * x + 2] = inPatch ? PATCH_RED : static_cast<uint8_t>((x + y) / 4);
            row[4 * x + 3] = 255;
        }
    }
}

void paint(AVFrame *picture, int index)
{
    av_frame_make_writable(picture);
    if (picture->format == AV_PIX_FMT_BGRA)
        paintBgra(picture, index);
    else
        paintYuv(picture, index);
}

// A page of text-like marks, dark on light, beside a coloured margin. All but its top third
// moves up four lines with every picture, as a page that is being read does. It is flat and
// sharp where the other picture is soft and noisy: what a shared screen mostly shows.
void paintPage(AVFrame *picture, int index)
{
    constexpr int CELL_WIDTH = 6;
    constexpr int CELL_HEIGHT = 14;
    constexpr int MARK_WIDTH = 5;
    constexpr int MARK_HEIGHT = 8;
    const int width = picture->width;
    const int height = picture->height;

    av_frame_make_writable(picture);
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        const int line = y < height / 3 ? y : y + 4 * index;
        for (int x = 0; x < width; x++) {
            uint32_t cell = (static_cast<uint32_t>(x / CELL_WIDTH) * 73856093u) ^ (static_cast<uint32_t>(line / CELL_HEIGHT) * 19349663u);
            cell ^= cell >> 13;
            cell *= 0x5bd1e995u;
            cell ^= cell >> 15;
            const int column = x % CELL_WIDTH;
            const int rowOfCell = line % CELL_HEIGHT;
            // One cell in five is the space between words.
            const bool written = (cell >> 24) % 5 != 0 && column < MARK_WIDTH && rowOfCell < MARK_HEIGHT;
            row[x] = written && ((cell >> (column + MARK_WIDTH * (rowOfCell / 2))) & 1) ? 30 : 225;
        }
    }

    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width / 2; x++) {
            const bool margin = x < width / 16;
            const uint8_t u = margin ? 110 : 128;
            const uint8_t v = margin ? 150 : 128;
            if (interleaved) {
                picture->data[1][y * picture->linesize[1] + 2 * x] = u;
                picture->data[1][y * picture->linesize[1] + 2 * x + 1] = v;
            } else {
                picture->data[1][y * picture->linesize[1] + x] = u;
                picture->data[2][y * picture->linesize[2] + x] = v;
            }
        }
    }
}

using Painter = void (*)(AVFrame *picture, int index);

AVFrame *allocatePicture(int format, int width, int height)
{
    AVFrame *picture = av_frame_alloc();
    picture->format = format;
    picture->width = width;
    picture->height = height;
    av_frame_get_buffer(picture, 0);
    return picture;
}

// How close decoded pictures are to the ones that were encoded: the PSNR of their luma, over
// the pictures from `from` on. A picture decoded larger than it was sent is compared over the
// part that was sent.
class Likeness
{
public:
    Likeness(Painter painter, const QSize &size, int from) : painter(painter), from(from), source(allocatePicture(AV_PIX_FMT_YUV420P, size.width(), size.height())) {}
    ~Likeness() { av_frame_free(&source); }

    Likeness(const Likeness &) = delete;
    Likeness &operator=(const Likeness &) = delete;

    void compare(const AVFrame *decoded, int picture)
    {
        const bool lumaInBytes = decoded->format == AV_PIX_FMT_YUV420P || decoded->format == AV_PIX_FMT_YUVJ420P || decoded->format == AV_PIX_FMT_NV12;
        if (picture < from || !lumaInBytes || decoded->width < source->width || decoded->height < source->height)
            return;
        painter(source, picture);
        for (int y = 0; y < source->height; y++) {
            const uint8_t *sent = source->data[0] + y * source->linesize[0];
            const uint8_t *seen = decoded->data[0] + y * decoded->linesize[0];
            for (int x = 0; x < source->width; x++)
                squaredError += static_cast<double>((sent[x] - seen[x]) * (sent[x] - seen[x]));
        }
        samples += static_cast<double>(source->width) * source->height;
    }

    // Zero when nothing was compared.
    [[nodiscard]] double psnr() const
    {
        if (samples == 0)
            return 0;
        return squaredError > 0 ? 10 * std::log10(255.0 * 255.0 * samples / squaredError) : 99;
    }

private:
    Painter painter;
    int from;
    AVFrame *source;
    double squaredError = 0;
    double samples = 0;
};

VideoEncoder::Settings settingsFor(const EncoderCase &encoderCase, int bitrate, int keyframeIntervalMs = 0)
{
    VideoEncoder::Settings settings;
    settings.codec = encoderCase.codec;
    settings.framerate = FRAMERATE;
    settings.bitrate = bitrate;
    settings.keyframeIntervalMs = keyframeIntervalMs;
    settings.encoderName = encoderCase.encoderName;
    return settings;
}

// One encoder fed synthetic pictures from memory, with what each call gave back.
class EncodeRun
{
public:
    EncodeRun(const EncoderCase &encoderCase, int width, int height, int bitrate, int keyframeIntervalMs = 0)
        : encoder(settingsFor(encoderCase, bitrate, keyframeIntervalMs)), picture(allocatePicture(encoder.softwareFormat(), width, height))
    {
    }

    ~EncodeRun() { av_frame_free(&picture); }

    void paintNext() { paint(picture, sent); }

    // False when the encoder could not be used at all.
    bool encodeNext(bool painting = true)
    {
        if (painting)
            paintNext();
        const size_t before = frames.size();
        const uint32_t timestamp = static_cast<uint32_t>(sent) * TICKS_PER_FRAME;

        QElapsedTimer timer;
        timer.start();
        const VideoEncoder::Outcome outcome = encoder.encode(picture, timestamp, frames);
        callNs.push_back(timer.nsecsElapsed());
        if (outcome != VideoEncoder::Outcome::Encoded)
            return false;

        // A frame belongs to the picture that went in with the same call, or it was held back.
        producedItsOwnFrame.push_back(frames.size() == before + 1 && frames.back().rtpTimestamp == timestamp);
        sent++;
        return true;
    }

    [[nodiscard]] double averageCallMs(size_t from) const
    {
        qint64 total = 0;
        for (size_t i = from; i < callNs.size(); i++)
            total += callNs[i];
        return callNs.size() > from ? static_cast<double>(total) / 1e6 / static_cast<double>(callNs.size() - from) : 0;
    }

    [[nodiscard]] double bitsPerSecond(size_t from, size_t to, int pictures) const
    {
        size_t bytes = 0;
        for (size_t i = from; i < to; i++)
            bytes += frames[i].data.size();
        return static_cast<double>(bytes) * 8.0 * FRAMERATE / pictures;
    }

    VideoEncoder encoder;
    std::vector<EncodedVideoFrame> frames;
    std::vector<bool> producedItsOwnFrame;
    std::vector<qint64> callNs;
    int sent = 0;

private:
    AVFrame *picture;
};

#ifdef Q_OS_WIN
// Synthetic pictures put on the graphics card the way captured ones are found there.
class CardPictures
{
public:
    CardPictures(AVPixelFormat format, int width, int height, int colorspace)
        : colorspace(colorspace), inMemory(allocatePicture(format, width, height)), onCard(av_frame_alloc())
    {
        AVBufferRef *device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
            return;
        frames = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);

        auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
        context->format = AV_PIX_FMT_D3D11;
        context->sw_format = format;
        context->width = width;
        context->height = height;
        static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
        if (av_hwframe_ctx_init(frames) < 0)
            av_buffer_unref(&frames);
    }

    ~CardPictures()
    {
        av_frame_free(&inMemory);
        av_frame_free(&onCard);
        av_buffer_unref(&frames);
    }

    [[nodiscard]] bool usable() const { return frames != nullptr; }

    AVFrame *next(int index, Painter painter = paint)
    {
        av_frame_make_writable(inMemory);
        painter(inMemory, index);
        av_frame_unref(onCard);
        if (av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0)
            return nullptr;
        onCard->colorspace = static_cast<AVColorSpace>(colorspace);
        onCard->color_range = AVCOL_RANGE_MPEG;
        return onCard;
    }

private:
    int colorspace;
    AVBufferRef *frames = nullptr;
    AVFrame *inMemory;
    AVFrame *onCard;
};

// One colour to each quarter of a picture, strong enough that the two sets of coefficients make
// something clearly different of three of them: top left, top right, bottom left, bottom right.
constexpr std::array QUARTER_COLOURS = { RGB(200, 40, 40), RGB(40, 200, 60), RGB(40, 60, 220), RGB(128, 128, 128) };

// A BGRA picture on the graphics card with one of the colours in each quarter. Its lower half
// has no opacity, as the bitmaps have that windows draw themselves into. Null when the card
// will not have it.
AVFrame *quartersOnCard(AVBufferRef *device, const QSize &size)
{
    AVFrame *inMemory = allocatePicture(AV_PIX_FMT_BGRA, size.width(), size.height());
    for (int y = 0; y < size.height(); y++) {
        uint8_t *row = inMemory->data[0] + y * inMemory->linesize[0];
        const bool lower = y >= size.height() / 2;
        for (int x = 0; x < size.width(); x++) {
            const COLORREF colour = QUARTER_COLOURS[(lower ? 2 : 0) + (x >= size.width() / 2 ? 1 : 0)];
            row[4 * x] = GetBValue(colour);
            row[4 * x + 1] = GetGValue(colour);
            row[4 * x + 2] = GetRValue(colour);
            row[4 * x + 3] = lower ? 0 : 255;
        }
    }

    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
    context->format = AV_PIX_FMT_D3D11;
    context->sw_format = AV_PIX_FMT_BGRA;
    context->width = size.width();
    context->height = size.height();
    static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;

    AVFrame *onCard = av_frame_alloc();
    if (av_hwframe_ctx_init(frames) < 0 || av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0)
        av_frame_free(&onCard);
    av_buffer_unref(&frames);
    av_frame_free(&inMemory);
    return onCard;
}

AVPixelFormat formatOnCard(const AVFrame *card)
{
    return card->hw_frames_ctx ? reinterpret_cast<const AVHWFramesContext *>(card->hw_frames_ctx->data)->sw_format : AV_PIX_FMT_NONE;
}

Yuv colourAt(const AVFrame *nv12, int x, int y)
{
    const uint8_t *chroma = nv12->data[1] + (y / 2) * nv12->linesize[1] + (x / 2) * 2;
    return { static_cast<double>(nv12->data[0][y * nv12->linesize[0] + x]), static_cast<double>(chroma[0]), static_cast<double>(chroma[1]) };
}

Yuv inYuv(COLORREF colour, bool bt709)
{
    return inYuv(GetRValue(colour), GetGValue(colour), GetBValue(colour), bt709);
}

// Whether the card makes NV12 textures of the kind a frames context of this shape asks for.
bool cardMakesNv12Textures(int poolSize, unsigned bindFlags)
{
    AVBufferRef *device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
        return false;
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);

    auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
    context->format = AV_PIX_FMT_D3D11;
    context->sw_format = AV_PIX_FMT_NV12;
    context->width = 1280;
    context->height = 720;
    context->initial_pool_size = poolSize;
    static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = bindFlags;
    bool made = av_hwframe_ctx_init(frames) >= 0;
    if (made) {
        AVFrame *frame = av_frame_alloc();
        made = av_hwframe_get_buffer(frames, frame, 0) >= 0;
        av_frame_free(&frame);
    }
    av_buffer_unref(&frames);
    return made;
}

// The processor time this process has used on all its threads, in milliseconds. Counted in
// processor cycles: the times Windows keeps are sampled at its clock tick, which misses most
// of the work of threads that wake up for a few milliseconds at a time.
double processCpuMs()
{
    static const double cyclesPerMs = [] {
        constexpr qint64 CALIBRATION_NS = 20'000'000;
        ULONG64 before = 0;
        ULONG64 after = 0;
        QueryThreadCycleTime(GetCurrentThread(), &before);
        QElapsedTimer spinning;
        spinning.start();
        while (spinning.nsecsElapsed() < CALIBRATION_NS) {}
        QueryThreadCycleTime(GetCurrentThread(), &after);
        return static_cast<double>(after - before) / (static_cast<double>(CALIBRATION_NS) / 1e6);
    }();

    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return static_cast<double>(cycles) / cyclesPerMs;
}
#else
double processCpuMs()
{
    return 0;
}
#endif

bool startsWithStartCode(const std::vector<uint8_t> &data)
{
    if (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;
    return data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

// The types of an Annex B frame's NAL units, as H264 or as H265 numbers them.
std::vector<int> nalTypes(VideoCodec codec, const std::vector<uint8_t> &data)
{
    std::vector<int> types;
    for (size_t i = 0; i + 3 < data.size(); i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            types.push_back(codec == VideoCodec::H265 ? (data[i + 3] >> 1) & 0x3F : data[i + 3] & 0x1F);
    }
    return types;
}

constexpr int H264_SPS = 7;
constexpr int H264_PPS = 8;
constexpr int H264_IDR = 5;
constexpr int H265_VPS = 32;
constexpr int H265_SPS = 33;
constexpr int H265_PPS = 34;
constexpr int H265_FIRST_IRAP = 16;
constexpr int H265_LAST_IRAP = 23;
constexpr int OBU_SEQUENCE_HEADER = 1;
constexpr int OBU_TEMPORAL_DELIMITER = 2;
constexpr int OBU_FRAME_HEADER = 3;
constexpr int OBU_FRAME = 6;

struct Obu
{
    int type = 0;
    bool saysItsSize = false;
    size_t payload = 0;
    size_t size = 0;
};

// The OBUs of an AV1 temporal unit, or none when it is not made of whole ones.
std::vector<Obu> obusOf(const std::vector<uint8_t> &data)
{
    std::vector<Obu> obus;
    for (size_t at = 0; at < data.size();) {
        Obu obu;
        obu.type = (data[at] >> 3) & 0x0F;
        obu.saysItsSize = data[at] & 0x02;
        obu.payload = at + ((data[at] & 0x04) ? 2 : 1);
        if ((data[at] & 0x80) || obu.payload > data.size())
            return {};
        obu.size = data.size() - obu.payload;
        if (obu.saysItsSize) {
            obu.size = 0;
            uint8_t byte = 0x80;
            for (int shift = 0; byte & 0x80; shift += 7) {
                if (obu.payload >= data.size() || shift > 28)
                    return {};
                byte = data[obu.payload++];
                obu.size |= size_t(byte & 0x7F) << shift;
            }
        }
        if (obu.size > data.size() - obu.payload)
            return {};
        obus.push_back(obu);
        at = obu.payload + obu.size;
    }
    return obus;
}

QString unitTypes(VideoCodec codec, const EncodedVideoFrame &frame)
{
    QStringList types;
    if (codec == VideoCodec::AV1) {
        for (const Obu &obu : obusOf(frame.data))
            types.append(QString::number(obu.type));
    } else if (codec != VideoCodec::VP8) {
        for (int type : nalTypes(codec, frame.data))
            types.append(QString::number(type));
    }
    return types.join(QLatin1Char(' '));
}

// What a key frame says its profile is: the profile_idc of an H264 SPS, the general_profile_idc
// of an H265 one, the seq_profile of an AV1 sequence header. Or -1.
int profileOf(VideoCodec codec, const std::vector<uint8_t> &data)
{
    if (codec == VideoCodec::AV1) {
        for (const Obu &obu : obusOf(data)) {
            if (obu.type == OBU_SEQUENCE_HEADER && obu.size > 0)
                return data[obu.payload] >> 5;
        }
        return -1;
    }
    for (size_t i = 0; i + 6 < data.size(); i++) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        if (codec == VideoCodec::H264 && (data[i + 3] & 0x1F) == H264_SPS)
            return data[i + 4];
        if (codec == VideoCodec::H265 && ((data[i + 3] >> 1) & 0x3F) == H265_SPS)
            return data[i + 6] & 0x1F;
    }
    return -1;
}

// A key frame of H264 or H265 is Annex B and opens with its parameter sets, one of AV1 has its
// sequence header before its picture and every OBU says its size, and a VP8 frame says itself
// what it is. Whether a frame is a key frame is checked against the frame.
bool isWellFormed(VideoCodec codec, const EncodedVideoFrame &frame)
{
    if (frame.data.empty())
        return false;
    if (codec == VideoCodec::VP8)
        return !(frame.data[0] & 1) == frame.keyframe;

    if (codec == VideoCodec::AV1) {
        bool hasSequenceHeader = false;
        const Obu *picture = nullptr;
        const std::vector<Obu> obus = obusOf(frame.data);
        for (const Obu &obu : obus) {
            if (!obu.saysItsSize)
                return false;
            if (obu.type == OBU_SEQUENCE_HEADER && !picture)
                hasSequenceHeader = true;
            if ((obu.type == OBU_FRAME || obu.type == OBU_FRAME_HEADER) && !picture)
                picture = &obu;
        }
        if (!picture || picture->size == 0)
            return false;
        // show_existing_frame, the two bits of frame_type, where zero is a key frame, and show_frame.
        const uint8_t opening = frame.data[picture->payload];
        const bool keyFrame = !(opening & 0x80) && ((opening >> 5) & 0x03) == 0 && (opening & 0x10);
        return keyFrame == frame.keyframe && (!keyFrame || hasSequenceHeader);
    }

    const bool h265 = codec == VideoCodec::H265;
    const std::vector<int> types = nalTypes(codec, frame.data);
    const bool hasKeyPicture = std::any_of(types.begin(), types.end(), [h265](int type) { return h265 ? type >= H265_FIRST_IRAP && type <= H265_LAST_IRAP : type == H264_IDR; });
    if (!startsWithStartCode(frame.data) || hasKeyPicture != frame.keyframe)
        return false;
    if (!frame.keyframe)
        return true;
    if (h265)
        return types.size() >= 4 && types[0] == H265_VPS && types[1] == H265_SPS && types[2] == H265_PPS;
    return types.size() >= 3 && types[0] == H264_SPS && types[1] == H264_PPS;
}

struct Decoded
{
    int pictures = 0;
    int width = 0;
    int height = 0;
    quint64 errors = 0;
    quint64 dropped = 0;
    int format = AV_PIX_FMT_NONE;
    int colorspace = AVCOL_SPC_UNSPECIFIED;
    int range = AVCOL_RANGE_UNSPECIFIED;
    // Of a sample: other than square when the stream wants its pictures shown at another size
    // than they were coded at.
    AVRational shape = { 0, 1 };
    Yuv middle;
    Yuv top;
};

// Whether what was decoded is the picture that was sent. A graphics card that codes AV1 only
// in whole blocks, as a Radeon of the 7000 series does in ones of 64 by 16, sends the picture
// with what fills its last blocks: larger than it was, and nothing in the stream says by how
// much. See whatAnEncoderMakesOfASizeItCannotCode.
bool isThePictureSent(VideoCodec codec, const Decoded &decoded, const QSize &sent)
{
    if (QSize(decoded.width, decoded.height) == sent)
        return true;
    return codec == VideoCodec::AV1 && decoded.width >= sent.width() && decoded.height >= sent.height() && decoded.width - sent.width() < 64 && decoded.height - sent.height() < 16;
}

// Decodes the frames in order, in software, and shows each picture to `inspect` with the
// place among the frames of the one it was decoded from.
template <typename Frames>
Decoded decodeAll(VideoCodec codec, const Frames &frames, std::function<void(const AVFrame *, int)> inspect = {})
{
    auto sink = std::make_shared<CountingSink>();
    sink->inspect = std::move(inspect);
    Core::Media::VideoDecoder decoder(false);
    decoder.setSink(sink);

    int pushed = 0;
    QElapsedTimer timer;
    timer.start();
    for (const auto &frame : frames) {
        decoder.push(codec, EncodedVideoFrame(*frame));
        pushed++;
        // The decoder throws its queue away when it falls far behind.
        while (pushed - sink->frames > 20 && timer.elapsed() < 10000)
            QThread::msleep(1);
    }
    while (sink->frames < pushed && timer.elapsed() < 10000)
        QThread::msleep(2);
    QThread::msleep(50);

    Decoded decoded;
    decoded.pictures = sink->frames;
    decoded.width = sink->width;
    decoded.height = sink->height;
    decoded.errors = decoder.stats().errors;
    decoded.dropped = decoder.stats().dropped;
    decoded.format = sink->format;
    decoded.colorspace = sink->colorspace;
    decoded.range = sink->range;
    decoded.shape = { sink->shapeWidth, sink->shapeHeight };
    decoded.middle = { static_cast<double>(sink->middleY), static_cast<double>(sink->middleU), static_cast<double>(sink->middleV) };
    decoded.top = { static_cast<double>(sink->topY), static_cast<double>(sink->topU), static_cast<double>(sink->topV) };
    return decoded;
}

Decoded decodeAll(VideoCodec codec, const std::vector<EncodedVideoFrame> &frames, std::function<void(const AVFrame *, int)> inspect = {})
{
    std::vector<const EncodedVideoFrame *> pointers;
    for (const EncodedVideoFrame &frame : frames)
        pointers.push_back(&frame);
    return decodeAll(codec, pointers, std::move(inspect));
}

// What a pipeline said while it ran, and when each of its frames came.
struct PipelineRecord
{
    explicit PipelineRecord(Core::Video::BroadcastPipeline &pipeline)
    {
        using Core::Video::BroadcastPipeline;
        clock.start();
        QObject::connect(&pipeline, &BroadcastPipeline::started, &here, [this](const QSize &size, int) { starts.append(size); });
        QObject::connect(&pipeline, &BroadcastPipeline::frameEncoded, &here, [this](const Core::Video::EncodedFramePointer &frame) {
            frames.push_back(frame);
            arrivedAt.push_back(clock.elapsed());
        });
        QObject::connect(&pipeline, &BroadcastPipeline::statsUpdated, &here, [this](const Core::Video::BroadcastStats &update) { stats.append(update); });
        QObject::connect(&pipeline, &BroadcastPipeline::captureStalled, &here, [this](bool stalled) { stalls.append(stalled); });
        QObject::connect(&pipeline, &BroadcastPipeline::sourceClosed, &here, [this] { closes++; });
        QObject::connect(&pipeline, &BroadcastPipeline::failed, &here, [this](const QString &reason) { failures.append(reason); });
    }

    [[nodiscard]] std::vector<Core::Video::EncodedFramePointer> framesBefore(size_t end) const
    {
        return std::vector<Core::Video::EncodedFramePointer>(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(end));
    }

    // The first key frame among those from `from` on, or the number of frames when there is none.
    [[nodiscard]] size_t keyFrameFrom(size_t from) const
    {
        size_t index = from;
        while (index < frames.size() && !frames[index]->keyframe)
            index++;
        return index;
    }

    QObject here;
    QElapsedTimer clock;
    std::vector<Core::Video::EncodedFramePointer> frames;
    std::vector<qint64> arrivedAt;
    QList<QSize> starts;
    QList<Core::Video::BroadcastStats> stats;
    QList<bool> stalls;
    QStringList failures;
    int closes = 0;
};

// How many of a duplicated window's pictures were cut out of the screen and how many the window
// drew itself, which the pipeline's statistics carry in the encoder's name. Negative when the
// window is not captured that way.
struct PathCounts
{
    qint64 cutOut = -1;
    qint64 drawn = -1;
};

PathCounts pathCounts(const Core::Video::BroadcastStats &stats)
{
    static const QRegularExpression pattern(QStringLiteral("ddagrab (\\d+) \\+ PrintWindow (\\d+) frames"));
    const QRegularExpressionMatch match = pattern.match(stats.encoderName);
    return match.hasMatch() ? PathCounts{ match.captured(1).toLongLong(), match.captured(2).toLongLong() } : PathCounts{};
}

// What a share whose pictures are this size offers to send with, best first: AV1's encoder is
// given no size it would add black to (thePipelineHoldsBackWhatItsEncoderWouldPad).
QList<VideoCodec> codecsSharedAt(const QSize &size)
{
    return Core::Media::shareCodecs(Core::Media::encodableVideoCodecs(), Core::Media::videoEncoderFor(VideoCodec::AV1), size);
}

// Whether a share's pictures reach the encoder the probe chose without leaving the graphics
// card: as they were captured where it takes those, scaled by graphics capture itself, or
// scaled and converted by the card's video processor.
bool reachesTheEncoderOnTheCard(VideoCodec codec, bool scaled, bool byGraphicsCapture)
{
    const VideoEncoder chosen(settingsFor({ codec, {} }, BITRATE));
    const bool withoutTheProcessor = chosen.takesRgbCardFrames() && (!scaled || byGraphicsCapture);
    return chosen.takesCardFrames() && (withoutTheProcessor || Core::Video::CardScaler::usable());
}

// Whether a screen of this machine is on a graphics adapter that gives a Direct3D 11 device,
// which capturing it takes either way. A machine that draws its screen in software, as a
// hosted build machine does, has no such adapter.
bool aScreenIsOnAGraphicsCard()
{
#ifdef Q_OS_WIN
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return false;

    bool found = false;
    IDXGIAdapter *adapter = nullptr;
    for (UINT index = 0; !found && SUCCEEDED(factory->EnumAdapters(index, &adapter)); index++) {
        IDXGIOutput *output = nullptr;
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
            output->Release();
            AVBufferRef *device = nullptr;
            found = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(index).constData(), nullptr, 0) >= 0;
            av_buffer_unref(&device);
        }
        adapter->Release();
    }
    factory->Release();
    return found;
#else
    return true;
#endif
}

#ifdef Q_OS_WIN
// The two solid colours of the windows the tests put on the screen: one to share and one to
// lay over it.
constexpr COLORREF SHARED_COLOUR = RGB(PATCH_RED, PATCH_GREEN, PATCH_BLUE);
constexpr COLORREF COVERING_COLOUR = RGB(220, 40, 200);
// The luma under which a decoded picture is neither of those: black, at 16 or 0.
constexpr int BLACK_AT_MOST = 40;

// An encoder may have been told either set of coefficients, depending on the picture's size.
bool looksLike(const Yuv &seen, COLORREF colour)
{
    constexpr double CLOSE = 16;
    return distance(seen, inYuv(GetRValue(colour), GetGValue(colour), GetBValue(colour), true)) < CLOSE || distance(seen, inYuv(GetRValue(colour), GetGValue(colour), GetBValue(colour), false)) < CLOSE;
}

// How far a decoded colour is from what `colour` comes to under the coefficients the stream is
// shown with: the ones it names, and failing that BT.709 from 720 lines up, as decoders have it.
double offItsOwnCoefficients(const Decoded &decoded, const Yuv &seen, COLORREF colour)
{
    const bool named = decoded.colorspace == AVCOL_SPC_BT709 || decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG;
    return distance(seen, inYuv(colour, named ? decoded.colorspace == AVCOL_SPC_BT709 : decoded.height >= 720));
}
// As far off as an encoder at a few megabits leaves a flat colour, and well short of the dozen
// and more the wrong coefficients put it off by.
constexpr double COLOUR_SURVIVES = 8;

// Makes the calling thread measure in the desktop's own pixels, or in the scaled ones Windows
// gives a program that knows nothing of display scaling.
void measureInDesktopPixels(bool desktopPixels)
{
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    static const auto setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    if (setAwareness)
        setAwareness(desktopPixels ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : DPI_AWARENESS_CONTEXT_UNAWARE);
}

// A screen other than the main one, which is where the tests put their windows: whoever is at
// the machine is at work on the main one.
struct QuietScreen
{
    // Both in the desktop's own pixels.
    QRect whole;
    QRect workArea;

    // Where a window goes that takes up this much room: the bottom right corner.
    [[nodiscard]] QPoint cornerFor(const QSize &room) const
    {
        constexpr int MARGIN = 24;
        return QPoint(std::max(workArea.left(), workArea.left() + workArea.width() - room.width() - MARGIN), std::max(workArea.top(), workArea.top() + workArea.height() - room.height() - MARGIN));
    }
};

BOOL CALLBACK takeQuietScreen(HMONITOR monitor, HDC, LPRECT, LPARAM found)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    // Not a turned one: desktop duplication hands such a screen over lying on its side.
    if (!GetMonitorInfoW(monitor, &info) || (info.dwFlags & MONITORINFOF_PRIMARY) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayOrientation != DMDO_DEFAULT)
        return TRUE;

    const auto rect = [](const RECT &native) { return QRect(native.left, native.top, native.right - native.left, native.bottom - native.top); };
    *reinterpret_cast<std::optional<QuietScreen> *>(found) = QuietScreen{ rect(info.rcMonitor), rect(info.rcWork) };
    return FALSE;
}

std::optional<QuietScreen> quietScreen()
{
    std::optional<QuietScreen> found;
    EnumDisplayMonitors(nullptr, nullptr, takeQuietScreen, reinterpret_cast<LPARAM>(&found));
    return found;
}

#define SKIP_WITHOUT_A_QUIET_SCREEN(screen)                  \
    const std::optional<QuietScreen> screen = quietScreen(); \
    if (!screen)                                             \
    QSKIP("the windows this opens go on a second screen, out of the way, and this machine has one")

// A top-level window of the test's own, on a thread of its own: one colour all over, and a
// small square in its corner that changes with every tick of a timer while it is animated.
// It stays above the windows already on its screen, so that those cannot cover it, has no
// button on the taskbar, and is shown without taking the keyboard from whoever is typing.
class TestWindow
{
public:
    struct Look
    {
        QSize inside;
        // Of the window's outside, in the desktop's own pixels. On a QuietScreen.
        QPoint at;
        COLORREF colour = SHARED_COLOUR;
        bool framed = true;
        // Made by a thread that knows nothing of display scaling, so that Windows scales it.
        bool scaledByWindows = false;
    };

    explicit TestWindow(const Look &look) : look(look), thread([this] { run(); })
    {
        while (!window && !failedToOpen)
            QThread::msleep(5);
        if (!window)
            return;
        if (look.framed)
            resizeInside(look.inside);
        moveTo(look.at);
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }

    ~TestWindow()
    {
        if (isOpen())
            PostMessageW(window, WM_CLOSE, 0, 0);
        thread.join();
    }

    [[nodiscard]] HWND handle() const { return window; }
    [[nodiscard]] bool isOpen() const { return window && IsWindow(window); }
    [[nodiscard]] QString sourceId() const { return QStringLiteral("window:%1").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(window.load()))); }

    // In the desktop's own pixels.
    [[nodiscard]] QRect insideOnScreen() const
    {
        RECT inside = {};
        POINT corner = {};
        GetClientRect(window, &inside);
        ClientToScreen(window, &corner);
        return QRect(corner.x, corner.y, inside.right, inside.bottom);
    }

    void setAnimated(bool on) { animated = on; }

    void resizeInside(const QSize &size)
    {
        RECT whole = {};
        GetWindowRect(window, &whole);
        const QSize grown = QSize(whole.right - whole.left, whole.bottom - whole.top) + size - insideOnScreen().size();
        SetWindowPos(window, nullptr, 0, 0, grown.width(), grown.height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void moveTo(const QPoint &at) { SetWindowPos(window, nullptr, at.x(), at.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
    void show() { ShowWindowAsync(window, SW_SHOWNOACTIVATE); }
    void hide() { ShowWindowAsync(window, SW_HIDE); }
    void close() { PostMessageW(window, WM_CLOSE, 0, 0); }
    // False where Windows will not have it.
    bool keepOutOfCaptures(bool keep) { return SetWindowDisplayAffinity(window, keep ? WDA_MONITOR : WDA_NONE) != FALSE; }

    // Puts the inside of the window this far past an edge of `screen` that has no other screen
    // beyond it. False when there is no such edge.
    bool reachPastAnOuterEdge(const QRect &screen, int pixels)
    {
        struct Edge
        {
            QPoint insideAt;
            QRect beyond;
        };
        constexpr int FRAME_ALLOWANCE = 64;
        const QRect inside = insideOnScreen();
        RECT whole = {};
        GetWindowRect(window, &whole);
        const QSize outer(whole.right - whole.left, whole.bottom - whole.top);
        const int reach = pixels + FRAME_ALLOWANCE;
        const int right = screen.left() + screen.width();
        const int bottom = screen.top() + screen.height();
        const std::array<Edge, 4> edges = {
            Edge{ QPoint(screen.left() - pixels, inside.top()), QRect(screen.left() - reach, whole.top, reach, outer.height()) },
            Edge{ QPoint(right + pixels - inside.width(), inside.top()), QRect(right, whole.top, reach, outer.height()) },
            Edge{ QPoint(inside.left(), screen.top() - pixels), QRect(whole.left, screen.top() - reach, outer.width(), reach) },
            Edge{ QPoint(inside.left(), bottom + pixels - inside.height()), QRect(whole.left, bottom, outer.width(), reach) },
        };
        for (const Edge &edge : edges) {
            const RECT beyond = { edge.beyond.left(), edge.beyond.top(), edge.beyond.left() + edge.beyond.width(), edge.beyond.top() + edge.beyond.height() };
            if (MonitorFromRect(&beyond, MONITOR_DEFAULTTONULL))
                continue;
            moveTo(edge.insideAt - (inside.topLeft() - QPoint(whole.left, whole.top)));
            return true;
        }
        return false;
    }

    // Kept from the screen by Windows at the program's own asking, as a window on another
    // virtual desktop is: still there and not minimised, and no longer drawn.
    void setCloaked(bool cloaked)
    {
        constexpr DWORD CLOAK_ATTRIBUTE = 13;
        using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        static const auto setAttribute = reinterpret_cast<SetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute")));
        const BOOL value = cloaked;
        if (setAttribute)
            setAttribute(window, CLOAK_ATTRIBUTE, &value, sizeof(value));
    }

private:
    static void fill(HDC context, const RECT &area, COLORREF colour)
    {
        const HBRUSH brush = CreateSolidBrush(colour);
        FillRect(context, &area, brush);
        DeleteObject(brush);
    }

    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        constexpr RECT CORNER = { 0, 0, 24, 24 };
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<const CREATESTRUCTW *>(lParam)->lpCreateParams));
        auto *self = reinterpret_cast<TestWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));

        switch (message) {
        case WM_TIMER:
            if (self && self->animated) {
                self->tick++;
                InvalidateRect(window, &CORNER, FALSE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT painting;
            const HDC context = BeginPaint(window, &painting);
            RECT inside = {};
            GetClientRect(window, &inside);
            if (self) {
                fill(context, inside, self->look.colour);
                fill(context, CORNER, RGB(self->tick * 37 & 255, self->tick * 91 & 255, self->tick * 13 & 255));
            }
            EndPaint(window, &painting);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void run()
    {
        static const wchar_t *const CLASS_NAME = L"AcheronCaptureTestWindow";
        measureInDesktopPixels(!look.scaledByWindows);

        WNDCLASSW description = {};
        description.style = CS_HREDRAW | CS_VREDRAW;
        description.lpfnWndProc = dispatch;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = CLASS_NAME;
        RegisterClassW(&description);

        // Its owner never shows, and is what keeps it off the taskbar. The test's own thread
        // puts it where it goes before showing it.
        const HWND owner = CreateWindowExW(0, CLASS_NAME, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, description.hInstance, nullptr);
        const HWND made = CreateWindowExW(WS_EX_TOPMOST, CLASS_NAME, L"Acheron capture test", look.framed ? WS_OVERLAPPEDWINDOW : WS_POPUP, look.at.x(), look.at.y(), look.inside.width(), look.inside.height(), owner, nullptr, description.hInstance, this);
        if (!made) {
            failedToOpen = true;
            return;
        }
        SetTimer(made, 1, 16, nullptr);
        window = made;

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    const Look look;
    std::atomic<HWND> window{ nullptr };
    std::atomic<bool> failedToOpen{ false };
    std::atomic<bool> animated{ true };
    int tick = 0;
    std::thread thread;
};
#endif

} // namespace

Q_DECLARE_METATYPE(EncoderCase)

// A named encoder the probe turned down is not one the rest of the tests can say anything about.
#define SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase)                                                     \
    if (!(encoderCase).encoderName.isEmpty() && !Core::Media::videoEncoderWorks((encoderCase).encoderName)) \
    QSKIP("this encoder does not open on this machine, or holds frames back")

class TestVideoEncode : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();

    void aSourceIsFittedInsideTheSendSize();
    void theProbeGivesTheSameAnswerEveryTime();
    void encodableCodecsAreOfferedForSending();
    void av1FromAmfIsCodedInWholeBlocks();
    void blackColumnsAreNotAcceptableAndAFewRowsAre();
    void av1IsOfferedOnlyWhereItAddsNoBlack();
    void theProbeTurnsDownWhatCannotKeepUp_data() { encoderCases(false); }
    void theProbeTurnsDownWhatCannotKeepUp();

    void everyPictureComesOutAtOnceAndDecodes_data() { encoderCases(true); }
    void everyPictureComesOutAtOnceAndDecodes();
    void aChangedBitrateIsFollowed_data() { encoderCases(true); }
    void aChangedBitrateIsFollowed();
    void keyFramesComeAtTheIntervalAskedFor_data() { encoderCases(true); }
    void keyFramesComeAtTheIntervalAskedFor();
    void aChangedPictureSizeStartsANewStream_data() { encoderCases(true); }
    void aChangedPictureSizeStartsANewStream();
    void encodeTimes_data() { encoderCases(true); }
    void encodeTimes();
    void aFrameIsMadeWholeWhateverItsEncoderLeftOut();
    void noKeyFrameComesUnasked_data() { encoderCases(false); }
    void noKeyFrameComesUnasked();
    void theStreamSaysWhatColoursItIsIn_data();
    void theStreamSaysWhatColoursItIsIn();
    void whatAnEncoderMakesOfASizeItCannotCode_data() { encoderCases(false); }
    void whatAnEncoderMakesOfASizeItCannotCode();
    void whatTheEncodersMakeOfTheSameRate_data();
    void whatTheEncodersMakeOfTheSameRate();
    void cleanupTestCase();

    void picturesOnTheGraphicsCardAreEncodedThere_data();
    void picturesOnTheGraphicsCardAreEncodedThere();
    void whichTexturesTheCardMakes();
    void theCardScalesAndConvertsPictures_data();
    void theCardScalesAndConvertsPictures();
    void picturesScaledOnTheCardAreEncodedThere_data();
    void picturesScaledOnTheCardAreEncodedThere();

    void screensAreListedWithThumbnails();
    void thePipelineSharesTheScreen_data();
    void thePipelineSharesTheScreen();
    void whatScalingAScreenCosts_data();
    void whatScalingAScreenCosts();
    void aScreenThatIsNotThereFails();

    void theCaptureMethodIsRemembered();
    void aStillPictureIsSentAgain();
    void aWindowAboveCoversTheOneBelow();
    void windowsAreListedWithoutThisProgramsOwn();
    void aWindowHasAThumbnail_data();
    void aWindowHasAThumbnail();
    void thePipelineSharesAWindow_data();
    void thePipelineSharesAWindow();
    void thePipelineHoldsBackWhatItsEncoderWouldPad();
    void whatCapturingAWindowCosts_data();
    void whatCapturingAWindowCosts();

private:
    static void encoderCases(bool withTheProbesChoice);

    // Where the settings the tests write go, in place of the registry.
    QTemporaryDir settingsDirectory;
    // What whatTheEncodersMakeOfTheSameRate measured, a line to each, for one table at the end.
    QStringList comparison;
};

Q_DECLARE_METATYPE(Core::Video::CaptureMethod)

void TestVideoEncode::encoderCases(bool withTheProbesChoice)
{
    QTest::addColumn<EncoderCase>("encoderCase");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : CODECS) {
        if (withTheProbesChoice && encodable.contains(codec))
            QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" as the probe chose"))) << EncoderCase{ codec, {} };
        for (const QString &name : Core::Media::videoEncoderNames(codec))
            QTest::newRow(qPrintable(name)) << EncoderCase{ codec, name };
    }
}

void TestVideoEncode::init()
{
    Core::Video::setCaptureMethod(Core::Video::CaptureMethod::Auto);
}

void TestVideoEncode::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("AcheronTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TestVideoEncode"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
#ifdef Q_OS_WIN
    measureInDesktopPixels(true);
#endif

    QStringList codecs;
    for (VideoCodec codec : Core::Media::encodableVideoCodecs())
        codecs.append(videoCodecName(codec));
    qInfo() << "encodable codecs:" << codecs;
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec))
            qInfo().noquote() << videoCodecName(codec) << "encoder" << name << (Core::Media::videoEncoderWorks(name) ? "works" : "is turned down by the probe");
    }
}

void TestVideoEncode::aSourceIsFittedInsideTheSendSize()
{
    using Core::Video::encodeSizeFor;
    QCOMPARE(encodeSizeFor(QSize(1920, 1080), 720), QSize(1280, 720));
    QCOMPARE(encodeSizeFor(QSize(2560, 1440), 1080), QSize(1920, 1080));
    QCOMPARE(encodeSizeFor(QSize(3840, 2160), 0), QSize(3840, 2160));
    // Never enlarged.
    QCOMPARE(encodeSizeFor(QSize(1280, 720), 1080), QSize(1280, 720));
    QCOMPARE(encodeSizeFor(QSize(1366, 768), 1080), QSize(1366, 768));
    // Wider than 16:9 is held by the width, taller by the height.
    QCOMPARE(encodeSizeFor(QSize(3440, 1440), 720), QSize(1280, 534));
    QCOMPARE(encodeSizeFor(QSize(1440, 2560), 720), QSize(404, 720));
    QCOMPARE(encodeSizeFor(QSize(1920, 1200), 720), QSize(1152, 720));
    // Both sides even, also when nothing is scaled.
    QCOMPARE(encodeSizeFor(QSize(1365, 767), 0), QSize(1364, 766));
    QCOMPARE(encodeSizeFor(QSize(1920, 1080), 480), QSize(852, 480));

#ifdef Q_OS_WIN
    if (!Core::Video::screenCaptureAvailable())
        QSKIP("this build cannot capture a screen");
    // A screen is as large as it was listed, and a window that is not there has no size.
    Core::Video::CaptureSource screen;
    screen.geometry = QRect(1920, 0, 2560, 1440);
    QCOMPARE(Core::Video::encodeSizeOf(screen, 1080), QSize(1920, 1080));
    QCOMPARE(Core::Video::encodeSizeOf(screen, 0), QSize(2560, 1440));
    QCOMPARE(Core::Video::encodeSizeOf(Core::Video::CaptureSource(), 720), QSize());
    Core::Video::CaptureSource noWindow;
    noWindow.kind = Core::Video::CaptureSource::Kind::Window;
    noWindow.id = QStringLiteral("window:1");
    QCOMPARE(Core::Video::encodeSizeOf(noWindow, 720), QSize());
#endif
}

void TestVideoEncode::theProbeGivesTheSameAnswerEveryTime()
{
    const QList<VideoCodec> first = Core::Media::encodableVideoCodecs();
    for (int i = 0; i < 3; i++)
        QVERIFY(Core::Media::encodableVideoCodecs() == first);
    QVERIFY(Core::Video::BroadcastPipeline::encodableCodecs() == first);

    // Best first, as the voice server has them: AV1, H265, H264, VP8.
    QList<VideoCodec> expected;
    for (VideoCodec codec : CODECS) {
        if (first.contains(codec))
            expected.append(codec);
    }
    QVERIFY(first == expected);

    // A codec is encodable exactly when one of its encoders works, and then it can be encoded.
    for (VideoCodec codec : CODECS) {
        const QList<QString> names = Core::Media::videoEncoderNames(codec);
        const bool oneWorks = std::any_of(names.begin(), names.end(), [](const QString &name) { return Core::Media::videoEncoderWorks(name); });
        QCOMPARE(first.contains(codec), oneWorks);

        EncodeRun run({ codec, {} }, WIDTH, HEIGHT, BITRATE);
        QCOMPARE(run.encoder.hasEncoder(), oneWorks);
        QCOMPARE(run.encodeNext(), oneWorks);

        // The encoder its pictures go to is the first of them that works.
        const QString chosen = Core::Media::videoEncoderFor(codec);
        const auto firstThatWorks = std::find_if(names.begin(), names.end(), [](const QString &name) { return Core::Media::videoEncoderWorks(name); });
        QCOMPARE(chosen, firstThatWorks == names.end() ? QString() : *firstThatWorks);
        if (oneWorks)
            QVERIFY2(run.encoder.stats().encoderName.startsWith(chosen), qPrintable(run.encoder.stats().encoderName));
    }
    EncodeRun unsupported({ VideoCodec::Unknown, {} }, WIDTH, HEIGHT, BITRATE);
    QVERIFY(!unsupported.encodeNext());

    // AV1 and H265 are the graphics card's or nobody's.
    for (VideoCodec codec : { VideoCodec::AV1, VideoCodec::H265 }) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            if (!Core::Media::videoEncoderWorks(name))
                continue;
            EncodeRun run({ codec, name }, WIDTH, HEIGHT, BITRATE);
            QVERIFY(run.encodeNext());
            QVERIFY2(run.encoder.stats().hardware, qPrintable(name));
        }
    }
}

void TestVideoEncode::encodableCodecsAreOfferedForSending()
{
    const QList<VideoCodec> decodable = Core::Media::decodableVideoCodecs();
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    const QList<Codec> offered = Core::Media::offeredVideoCodecs(encodable);
    QCOMPARE(offered.size(), decodable.size());
    for (int i = 0; i < offered.size(); i++) {
        QCOMPARE(offered[i].name.get(), videoCodecName(decodable[i]));
        QCOMPARE(offered[i].decode.get(), true);
        QCOMPARE(offered[i].encode.get(), encodable.contains(decodable[i]));
        QCOMPARE(offered[i].payloadType.get(), 101 + 2 * i);
    }
    for (const Codec &codec : Core::Media::offeredVideoCodecs())
        QCOMPARE(codec.encode.get(), false);
}

// What a Radeon RX 7000 was measured to make of each size (whatAnEncoderMakesOfASizeItCannotCode
// measures it again wherever the encoder works).
void TestVideoEncode::av1FromAmfIsCodedInWholeBlocks()
{
    using Core::Media::codedSize;
    const QString amf = QStringLiteral("av1_amf");
    QCOMPARE(codedSize(amf, QSize(1920, 1080)), QSize(1920, 1082));
    QCOMPARE(codedSize(amf, QSize(640, 360)), QSize(640, 362));
    QCOMPARE(codedSize(amf, QSize(960, 540)), QSize(960, 544));
    QCOMPARE(codedSize(amf, QSize(1000, 700)), QSize(1024, 704));
    QCOMPARE(codedSize(amf, QSize(1280, 720)), QSize(1280, 720));
    QCOMPARE(codedSize(amf, QSize(2560, 1440)), QSize(2560, 1440));
    QCOMPARE(codedSize(amf, QSize(1452, 752)), QSize(1472, 752));

    // Every other encoder codes what it is given.
    QStringList others = { QString(), QStringLiteral("av1_nvenc"), QStringLiteral("av1_qsv"), QStringLiteral("av1_mf hardware"), QStringLiteral("av1_vaapi") };
    for (VideoCodec codec : CODECS)
        others.append(Core::Media::videoEncoderNames(codec));
    others.removeAll(amf);
    QCOMPARE(others.contains(QStringLiteral("libvpx")), avcodec_find_encoder_by_name("libvpx") != nullptr);
    for (const QString &name : std::as_const(others)) {
        for (const QSize &size : { QSize(1920, 1080), QSize(640, 360), QSize(1000, 700), QSize(1452, 752) })
            QVERIFY2(codedSize(name, size) == size, qPrintable(name));
    }
}

void TestVideoEncode::blackColumnsAreNotAcceptableAndAFewRowsAre()
{
    using Core::Media::paddingIsAcceptable;
    QVERIFY(paddingIsAcceptable(QSize(1280, 720), QSize(1280, 720)));
    QVERIFY(paddingIsAcceptable(QSize(1280, 720), QSize(1280, 728)));
    QVERIFY(!paddingIsAcceptable(QSize(1280, 720), QSize(1280, 730)));
    QVERIFY(!paddingIsAcceptable(QSize(1280, 720), QSize(1282, 720)));
    QVERIFY(!paddingIsAcceptable(QSize(1280, 720), QSize(1344, 722)));

    const auto accepted = [](const QString &encoder, const QSize &size) { return paddingIsAcceptable(size, Core::Media::codedSize(encoder, size)); };
    const QString amf = QStringLiteral("av1_amf");
    QVERIFY(accepted(amf, QSize(1920, 1080)));
    QVERIFY(accepted(amf, QSize(640, 360)));
    QVERIFY(accepted(amf, QSize(960, 540)));
    QVERIFY(accepted(amf, QSize(1280, 720)));
    QVERIFY(accepted(amf, QSize(2560, 1440)));
    QVERIFY(!accepted(amf, QSize(1000, 700)));
    QVERIFY(!accepted(amf, QSize(1452, 752)));
    // The right width, and fourteen rows short of a whole block.
    QVERIFY(!accepted(amf, QSize(1280, 722)));

    for (const QString &exact : { QStringLiteral("av1_nvenc"), QStringLiteral("av1_qsv"), QStringLiteral("hevc_amf"), QStringLiteral("libvpx") }) {
        for (const QSize &size : { QSize(1920, 1080), QSize(1000, 700), QSize(1452, 752), QSize(1280, 722) })
            QVERIFY2(accepted(exact, size), qPrintable(exact));
    }
}

void TestVideoEncode::av1IsOfferedOnlyWhereItAddsNoBlack()
{
    using Core::Media::cameraCodecs;
    using Core::Media::shareCodecs;
    const QString amf = QStringLiteral("av1_amf");
    const QString nvenc = QStringLiteral("av1_nvenc");
    const QList<VideoCodec> all = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };
    const QList<VideoCodec> withoutAv1 = { VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

    QVERIFY(shareCodecs(all, amf, QSize(1920, 1080)) == all);
    QVERIFY(shareCodecs(all, amf, QSize(1280, 720)) == all);
    QVERIFY(shareCodecs(all, amf, QSize(1452, 752)) == withoutAv1);
    QVERIFY(shareCodecs(all, amf, QSize(1000, 700)) == withoutAv1);
    QVERIFY(shareCodecs(all, nvenc, QSize(1452, 752)) == all);
    QVERIFY(shareCodecs(withoutAv1, amf, QSize(1452, 752)) == withoutAv1);
    // Asked of what a share already offers, a codec that was left out once stays out.
    QVERIFY(shareCodecs(shareCodecs(all, amf, QSize(1452, 752)), amf, QSize(1920, 1080)) == withoutAv1);

    QVERIFY(cameraCodecs(all, amf) == withoutAv1);
    QVERIFY(cameraCodecs(all, nvenc) == all);
    QVERIFY(cameraCodecs(all, QStringLiteral("av1_qsv")) == all);
    QVERIFY(cameraCodecs(withoutAv1, amf) == withoutAv1);
    QVERIFY(cameraCodecs({ VideoCodec::VP8 }, amf) == QList<VideoCodec>{ VideoCodec::VP8 });
}

// The probe's verdict on an encoder is that it opens, gives every frame back with the picture
// it was made from, and makes about the bit rate it was asked for. Microsoft's software H264
// encoder opens and then runs sixteen pictures behind, which is why it is not used, and a
// Radeon's AV1 encoder never answers at all when it is reached through Media Foundation.
void TestVideoEncode::theProbeTurnsDownWhatCannotKeepUp()
{
    QFETCH(EncoderCase, encoderCase);
    constexpr int PICTURES = 30;
    constexpr int SETTLING = 10;
    constexpr int PROBE_BITRATE = 1'000'000;
    constexpr int ANSWERS_WITHIN_MS = 15000;

    struct Seen
    {
        bool opens = true;
        int frames = 0;
        int sent = 0;
        int withTheirPicture = 0;
        bool startsWithAKeyFrame = false;
        double rate = 0;
    };
    // On a thread that is left behind should the encoder never come back.
    const auto seen = std::make_shared<std::promise<Seen>>();
    std::future<Seen> finished = seen->get_future();
    std::thread([seen, encoderCase] {
        Seen result;
        EncodeRun run(encoderCase, WIDTH, HEIGHT, PROBE_BITRATE);
        size_t settled = 0;
        for (int index = 0; index < PICTURES && result.opens; index++) {
            if (index == SETTLING)
                settled = run.frames.size();
            result.opens = run.encodeNext();
        }
        result.frames = static_cast<int>(run.frames.size());
        result.sent = run.sent;
        result.withTheirPicture = static_cast<int>(std::count(run.producedItsOwnFrame.begin(), run.producedItsOwnFrame.end(), true));
        result.startsWithAKeyFrame = !run.frames.empty() && run.frames[0].keyframe;
        result.rate = run.bitsPerSecond(settled, run.frames.size(), PICTURES - SETTLING);
        seen->set_value(result);
    }).detach();

    if (finished.wait_for(std::chrono::milliseconds(ANSWERS_WITHIN_MS)) != std::future_status::ready) {
        qInfo().noquote() << encoderCase.encoderName << "never answers";
        QVERIFY(!Core::Media::videoEncoderWorks(encoderCase.encoderName));
        return;
    }
    const Seen result = finished.get();
    qInfo().noquote() << encoderCase.encoderName << (result.opens ? "opens;" : "does not open;") << result.frames << "frames from" << result.sent << "pictures," << result.withTheirPicture << "of them with the picture's own call; asked" << PROBE_BITRATE << "made" << qRound(result.rate);

    const bool keepsUp = result.opens && result.withTheirPicture == PICTURES && result.startsWithAKeyFrame;
    const bool holdsTheRate = result.rate > PROBE_BITRATE * 0.25 && result.rate < PROBE_BITRATE * 2.0;
    QCOMPARE(Core::Media::videoEncoderWorks(encoderCase.encoderName), keepsUp && holdsTheRate);
}

void TestVideoEncode::everyPictureComesOutAtOnceAndDecodes()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 75;
    const std::vector<int> forced = { 20, 21, 47 };

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    for (int index = 0; index < PICTURES; index++) {
        if (std::find(forced.begin(), forced.end(), index) != forced.end())
            run.encoder.requestKeyframe();
        QVERIFY2(run.encodeNext(), qPrintable(QStringLiteral("picture %1 could not be encoded").arg(index)));
    }
    qInfo() << "encoder:" << run.encoder.stats().encoderName << "hardware:" << run.encoder.stats().hardware;
    qInfo().noquote() << "the first key frame is made of units" << unitTypes(encoderCase.codec, run.frames[0]) << "and the frame after it of" << unitTypes(encoderCase.codec, run.frames[1]);
    if (encoderCase.codec != VideoCodec::VP8) {
        // The official client's: Main or High for H264 and not Baseline, Main for the others.
        const int profile = profileOf(encoderCase.codec, run.frames[0].data);
        qInfo() << "profile:" << profile;
        QVERIFY(encoderCase.codec == VideoCodec::H264 ? profile == 77 || profile == 100 : profile == (encoderCase.codec == VideoCodec::H265 ? 1 : 0));
    }

    // One picture in, its frame out of the same call: nothing is held back.
    QCOMPARE(run.frames.size(), size_t(PICTURES));
    for (int index = 0; index < PICTURES; index++)
        QVERIFY2(run.producedItsOwnFrame[static_cast<size_t>(index)], qPrintable(QStringLiteral("picture %1 did not come out with its own call").arg(index)));
    QCOMPARE(run.encoder.stats().opened, quint64(1));
    QCOMPARE(run.encoder.stats().encoded, quint64(PICTURES));

    // Key frames where they were asked for and nowhere else: none come by themselves.
    for (int index = 0; index < PICTURES; index++) {
        const bool expected = index == 0 || std::find(forced.begin(), forced.end(), index) != forced.end();
        QVERIFY2(run.frames[static_cast<size_t>(index)].keyframe == expected, qPrintable(QStringLiteral("frame %1: key frame %2, expected %3").arg(index).arg(run.frames[static_cast<size_t>(index)].keyframe).arg(expected)));
    }
    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));

    const Decoded whole = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(whole.errors, quint64(0));
    QCOMPARE(whole.dropped, quint64(0));
    QCOMPARE(whole.pictures, PICTURES);
    QVERIFY2(isThePictureSent(encoderCase.codec, whole, QSize(WIDTH, HEIGHT)), qPrintable(QStringLiteral("%1x%2").arg(whole.width).arg(whole.height)));
    // Eight bits and 4:2:0, as the official client sends.
    QVERIFY(whole.format == AV_PIX_FMT_YUV420P || whole.format == AV_PIX_FMT_YUVJ420P);

    // A key frame needs nothing that came before it, and what follows needs only the key frame.
    for (size_t index = 0; index < run.frames.size(); index++) {
        if (!run.frames[index].keyframe)
            continue;
        const Decoded alone = decodeAll(encoderCase.codec, std::vector<EncodedVideoFrame>{ run.frames[index] });
        QCOMPARE(alone.errors, quint64(0));
        QCOMPARE(alone.pictures, 1);
        QVERIFY(isThePictureSent(encoderCase.codec, alone, QSize(WIDTH, HEIGHT)));

        const std::vector<EncodedVideoFrame> fromHere(run.frames.begin() + static_cast<std::ptrdiff_t>(index), run.frames.end());
        const Decoded rest = decodeAll(encoderCase.codec, fromHere);
        QCOMPARE(rest.errors, quint64(0));
        QCOMPARE(rest.pictures, static_cast<int>(fromHere.size()));
    }
}

void TestVideoEncode::aChangedBitrateIsFollowed()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES_PER_RATE = 90;
    constexpr int SETTLING = 30;
    constexpr int LOWER_BITRATE = 500'000;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    size_t settledBefore = 0;
    for (int index = 0; index < PICTURES_PER_RATE; index++) {
        if (index == SETTLING)
            settledBefore = run.frames.size();
        QVERIFY(run.encodeNext());
    }
    const size_t atChange = run.frames.size();

    run.encoder.setBitrate(LOWER_BITRATE);
    size_t settledAfter = 0;
    for (int index = 0; index < PICTURES_PER_RATE; index++) {
        if (index == SETTLING)
            settledAfter = run.frames.size();
        QVERIFY(run.encodeNext());
    }

    // Measured against the pictures that went in, so that a dropped one counts as no bits.
    const double before = run.bitsPerSecond(settledBefore, atChange, PICTURES_PER_RATE - SETTLING);
    const double after = run.bitsPerSecond(settledAfter, run.frames.size(), PICTURES_PER_RATE - SETTLING);
    qInfo() << "encoder:" << run.encoder.stats().encoderName << "asked" << BITRATE << "made" << qRound(before) << "; asked" << LOWER_BITRATE << "made" << qRound(after) << "; opened" << run.encoder.stats().opened << "time(s);" << run.frames.size() << "frames from" << run.sent << "pictures";

    // The pictures are hard enough to use up either rate, so the stream sits near each.
    QVERIFY2(before > BITRATE * 0.7 && before < BITRATE * 1.3, qPrintable(QString::number(before)));
    QVERIFY2(after > LOWER_BITRATE * 0.7 && after < LOWER_BITRATE * 1.3, qPrintable(QString::number(after)));

    // An encoder that had to start over for the new rate starts with a key frame, and either
    // way everything that came out decodes as one stream.
    if (run.encoder.stats().opened > 1)
        QVERIFY(run.frames[atChange].keyframe);
    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));
    const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(run.frames.size()));
}

void TestVideoEncode::keyFramesComeAtTheIntervalAskedFor()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int INTERVAL_MS = 500;
    constexpr int PICTURES = 50;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE, INTERVAL_MS);
    for (int index = 0; index < PICTURES; index++)
        QVERIFY(run.encodeNext());

    QCOMPARE(run.frames.size(), size_t(PICTURES));
    const int framesPerInterval = INTERVAL_MS * FRAMERATE / 1000;
    for (int index = 0; index < PICTURES; index++)
        QCOMPARE(run.frames[static_cast<size_t>(index)].keyframe, index % framesPerInterval == 0);
}

void TestVideoEncode::aChangedPictureSizeStartsANewStream()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 10;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    QVERIFY(run.encodeNext());

    // The same encoder is handed pictures of another size, as after a change of screen mode.
    AVFrame *larger = allocatePicture(run.encoder.softwareFormat(), 960, 540);
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        paint(larger, index);
        QVERIFY(run.encoder.encode(larger, (index + 1) * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
    }
    av_frame_free(&larger);

    QCOMPARE(frames.size(), size_t(PICTURES));
    QVERIFY(frames[0].keyframe);
    QVERIFY(!frames[1].keyframe);
    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, PICTURES);
    QVERIFY2(isThePictureSent(encoderCase.codec, decoded, QSize(960, 540)), qPrintable(QStringLiteral("%1x%2").arg(decoded.width).arg(decoded.height)));
}

void TestVideoEncode::encodeTimes()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 60;
    constexpr int OPENING = 1;

    for (const QSize &size : { QSize(1280, 720), QSize(1920, 1080) }) {
        EncodeRun run(encoderCase, size.width(), size.height(), size.height() >= 1080 ? 6'000'000 : 3'500'000);
        QVERIFY(run.encodeNext());
        // Painting a picture is this test's own work and is kept out of the processor time.
        double processorMs = 0;
        for (int index = OPENING; index < PICTURES; index++) {
            run.paintNext();
            const double before = processCpuMs();
            QVERIFY(run.encodeNext(false));
            processorMs += processCpuMs() - before;
        }
        qInfo().noquote() << QStringLiteral("%1 %2x%3 from memory: %4 ms a picture, %5 ms of it processor time, opening took %6 ms").arg(run.encoder.stats().encoderName).arg(size.width()).arg(size.height()).arg(run.averageCallMs(OPENING), 0, 'f', 2).arg(processorMs / (PICTURES - OPENING), 0, 'f', 2).arg(static_cast<double>(run.callNs[0]) / 1e6, 0, 'f', 0);
        // A picture every 33 ms leaves no room for an encoder slower than that.
        QVERIFY(run.averageCallMs(OPENING) < 1000.0 / FRAMERATE);
    }
}

namespace {

std::vector<uint8_t> joined(const std::vector<std::vector<uint8_t>> &parts)
{
    std::vector<uint8_t> whole;
    for (const std::vector<uint8_t> &part : parts)
        whole.insert(whole.end(), part.begin(), part.end());
    return whole;
}

std::vector<uint8_t> annexB(const std::vector<std::vector<uint8_t>> &nals)
{
    std::vector<uint8_t> frame;
    for (const std::vector<uint8_t> &nal : nals) {
        frame.insert(frame.end(), { 0, 0, 0, 1 });
        frame.insert(frame.end(), nal.begin(), nal.end());
    }
    return frame;
}

// An OBU that says its size, or one that leaves it out, as the last of a temporal unit may.
std::vector<uint8_t> obu(int type, const std::vector<uint8_t> &payload, bool saysItsSize = true)
{
    std::vector<uint8_t> whole = { static_cast<uint8_t>((type << 3) | (saysItsSize ? 0x02 : 0)) };
    for (size_t size = payload.size(); saysItsSize; size >>= 7) {
        whole.push_back(static_cast<uint8_t>((size & 0x7F) | (size > 0x7F ? 0x80 : 0)));
        if (size <= 0x7F)
            break;
    }
    whole.insert(whole.end(), payload.begin(), payload.end());
    return whole;
}

struct Finished
{
    bool whole = false;
    bool keyframe = false;
    std::vector<uint8_t> data;
};

Finished finish(Core::Media::EncodedFrameFinisher &finisher, const std::vector<uint8_t> &data)
{
    EncodedVideoFrame frame;
    frame.data = data;
    Finished finished;
    finished.whole = finisher.finish(frame);
    finished.keyframe = frame.keyframe;
    finished.data = frame.data;
    return finished;
}

void remember(Core::Media::EncodedFrameFinisher &finisher, const std::vector<uint8_t> &extradata)
{
    finisher.rememberParameterSets(extradata.data(), extradata.size());
}

// Stamps one colour over the middle of a YUV picture.
void stampPatch(AVFrame *picture, const Yuv &colour)
{
    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = picture->height * 3 / 8; y < picture->height * 5 / 8; y++) {
        for (int x = picture->width * 3 / 8; x < picture->width * 5 / 8; x++) {
            picture->data[0][y * picture->linesize[0] + x] = static_cast<uint8_t>(std::lround(colour.y));
            if (interleaved) {
                picture->data[1][(y / 2) * picture->linesize[1] + (x / 2) * 2] = static_cast<uint8_t>(std::lround(colour.u));
                picture->data[1][(y / 2) * picture->linesize[1] + (x / 2) * 2 + 1] = static_cast<uint8_t>(std::lround(colour.v));
            } else {
                picture->data[1][(y / 2) * picture->linesize[1] + x / 2] = static_cast<uint8_t>(std::lround(colour.u));
                picture->data[2][(y / 2) * picture->linesize[2] + x / 2] = static_cast<uint8_t>(std::lround(colour.v));
            }
        }
    }
}

} // namespace

// The encoders of the machine this was written on leave nothing out, so the frames here are
// made by hand, of units that are little more than their first bytes: those are what say what
// a unit is.
void TestVideoEncode::aFrameIsMadeWholeWhateverItsEncoderLeftOut()
{
    using Core::Media::EncodedFrameFinisher;
    {
        const std::vector<uint8_t> sps = { 0x67, 0x64, 0x1F }, pps = { 0x68, 0xEE }, laterSps = { 0x67, 0x4D, 0x28 }, laterPps = { 0x68, 0xCE };
        const std::vector<uint8_t> delimiter = { 0x09, 0x10 }, idr = { 0x65, 0x88, 0x80 }, slice = { 0x41, 0x9A, 0x20 };
        EncodedFrameFinisher finisher(VideoCodec::H264);
        // A key frame whose parameter sets were never seen cannot be sent.
        QVERIFY(!finish(finisher, annexB({ idr })).whole);

        remember(finisher, annexB({ sps, pps }));
        Finished finished = finish(finisher, annexB({ delimiter, idr }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ sps, pps, idr }));

        finished = finish(finisher, annexB({ delimiter, slice }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == annexB({ delimiter, slice }));

        // A key frame that brings them is left as it is, and they are what the next one gets.
        finished = finish(finisher, annexB({ laterSps, laterPps, idr }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ laterSps, laterPps, idr }));
        QVERIFY(finish(finisher, annexB({ idr })).data == annexB({ laterSps, laterPps, idr }));

        // With lengths in place of start codes, as in an MP4 file.
        QVERIFY(!finish(finisher, { 0, 0, 0, 3, 0x65, 0x88, 0x80 }).whole);
        QVERIFY(!finish(finisher, {}).whole);
    }
    {
        const std::vector<uint8_t> vps = { 0x40, 0x01, 0x0C }, sps = { 0x42, 0x01, 0x01 }, pps = { 0x44, 0x01, 0xC1 }, laterSps = { 0x42, 0x01, 0x02 }, laterPps = { 0x44, 0x01, 0xC2 };
        const std::vector<uint8_t> delimiter = { 0x46, 0x01, 0x50 }, sei = { 0x4E, 0x01, 0x05 }, idr = { 0x26, 0x01, 0xAF }, cleanRandomAccess = { 0x2A, 0x01, 0xAF }, trailing = { 0x02, 0x01, 0xD0 };
        EncodedFrameFinisher finisher(VideoCodec::H265);
        remember(finisher, annexB({ sps, pps }));
        QVERIFY(!finish(finisher, annexB({ idr })).whole);

        remember(finisher, annexB({ vps, sps, pps }));
        Finished finished = finish(finisher, annexB({ delimiter, sei, idr }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ vps, sps, pps, sei, idr }));

        // Every picture a decoder can start at is a key frame, not only an IDR.
        finished = finish(finisher, annexB({ cleanRandomAccess }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ vps, sps, pps, cleanRandomAccess }));

        finished = finish(finisher, annexB({ delimiter, trailing }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == annexB({ delimiter, trailing }));

        QVERIFY(finish(finisher, annexB({ vps, sps, pps, idr })).data == annexB({ vps, sps, pps, idr }));
        // One that brings some of them gets the rest, in the order they belong in.
        QVERIFY(finish(finisher, annexB({ laterSps, laterPps, idr })).data == annexB({ vps, laterSps, laterPps, idr }));
    }
    {
        constexpr int TEMPORAL_DELIMITER = OBU_TEMPORAL_DELIMITER;
        constexpr int TILE_GROUP = 4;
        const std::vector<uint8_t> sequence = { 0x00, 0x00, 0x00, 0x42, 0xAB }, laterSequence = { 0x00, 0x00, 0x00, 0x6A, 0xAB };
        // What a frame header opens with: a key frame that is shown, a frame predicted from
        // others, and an earlier frame shown again.
        const std::vector<uint8_t> key = { 0x10, 0x00, 0x55 }, predicted = { 0x30, 0x11 }, shownAgain = { 0x90 };
        const std::vector<uint8_t> delimiter = obu(TEMPORAL_DELIMITER, {});
        EncodedFrameFinisher finisher(VideoCodec::AV1);
        QVERIFY(!finish(finisher, joined({ delimiter, obu(OBU_FRAME, key) })).whole);

        remember(finisher, obu(OBU_SEQUENCE_HEADER, sequence));
        Finished finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, key) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME, key) }));

        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, predicted) }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_FRAME, predicted) }));
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME_HEADER, shownAgain) }));
        QVERIFY(finished.whole && !finished.keyframe);

        // A frame may come as its header and its tiles apart.
        finished = finish(finisher, joined({ obu(OBU_FRAME_HEADER, key), obu(TILE_GROUP, { 0x77, 0x78 }) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME_HEADER, key), obu(TILE_GROUP, { 0x77, 0x78 }) }));

        finished = finish(finisher, joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));
        QVERIFY(finish(finisher, obu(OBU_FRAME, key)).data == joined({ obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));

        // The last OBU may leave its size out, and is given it: long enough here that the size
        // takes two bytes.
        std::vector<uint8_t> longKey(300, 0x5A);
        longKey[0] = key[0];
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, longKey, false) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, longKey) }));
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, predicted, false) }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_FRAME, predicted) }));

        // Not OBUs: the bit that is always clear is set, a size runs past the end, no picture.
        QVERIFY(!finish(finisher, { 0xB2, 0x01, 0x10 }).whole);
        QVERIFY(!finish(finisher, { 0x32, 0x09, 0x10 }).whole);
        QVERIFY(!finish(finisher, delimiter).whole);

        // Extradata may be the sequence header as an MP4 file keeps it, behind four bytes.
        EncodedFrameFinisher fromConfiguration(VideoCodec::AV1);
        remember(fromConfiguration, joined({ { 0x81, 0x00, 0x0C, 0x00 }, obu(OBU_SEQUENCE_HEADER, sequence) }));
        QVERIFY(finish(fromConfiguration, obu(OBU_FRAME, key)).data == joined({ obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME, key) }));

        // A stream of still pictures has key frames only, and its frame headers do not say so.
        const std::vector<uint8_t> stills = { 0x18, 0x00 };
        finished = finish(fromConfiguration, joined({ obu(OBU_SEQUENCE_HEADER, stills), obu(OBU_FRAME, predicted) }));
        QVERIFY(finished.whole && finished.keyframe);
    }
    {
        EncodedFrameFinisher finisher(VideoCodec::VP8);
        QVERIFY(finish(finisher, { 0x10, 0x02, 0x00 }).keyframe);
        Finished finished = finish(finisher, { 0x11, 0x02, 0x00 });
        QVERIFY(finished.whole && !finished.keyframe);
        EncodedFrameFinisher unknown;
        QVERIFY(!finish(unknown, { 0x10, 0x02, 0x00 }).whole);
    }
}

// A graphics card's encoder left to itself makes a key frame every few hundred pictures (a
// Radeon's every 240 or 300), which the other tests are too short to see.
void TestVideoEncode::noKeyFrameComesUnasked()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 330;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    for (int index = 0; index < PICTURES; index++)
        QVERIFY(run.encodeNext());
    QVERIFY(run.frames[0].keyframe);
    QCOMPARE(static_cast<int>(std::count_if(run.frames.begin(), run.frames.end(), [](const EncodedVideoFrame &frame) { return frame.keyframe; })), 1);
    QCOMPARE(run.encoder.stats().opened, quint64(1));
}

void TestVideoEncode::theStreamSaysWhatColoursItIsIn_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            QTest::newRow(qPrintable(name + QStringLiteral(", BT.709"))) << EncoderCase{ codec, name, AVCOL_SPC_BT709 };
            QTest::newRow(qPrintable(name + QStringLiteral(", BT.601"))) << EncoderCase{ codec, name, AVCOL_SPC_SMPTE170M };
        }
    }
}

// Pictures that are YUV already are not converted by an encoder, only described. A colour has
// to come out as it went in, in video range, and the stream has to name the matrix it is in or
// name none: a viewer takes a stream that names none for BT.709 from 720 lines up and BT.601
// below, which is why the pipelines send what a viewer would assume.
void TestVideoEncode::theStreamSaysWhatColoursItIsIn()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 20;
    const auto sent = static_cast<AVColorSpace>(encoderCase.detail);
    const Yuv patch = patchInYuv(sent == AVCOL_SPC_BT709);

    VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
    AVFrame *picture = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
    const auto freePicture = qScopeGuard([&picture] { av_frame_free(&picture); });
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        av_frame_make_writable(picture);
        paintYuv(picture, index);
        stampPatch(picture, patch);
        picture->colorspace = sent;
        picture->color_range = AVCOL_RANGE_MPEG;
        QVERIFY(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
    }

    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames.size()));
    qInfo().noquote() << QStringLiteral("%1: a patch of %2 %3 %4 decodes to %5 %6 %7; the stream was told matrix %8 and names matrix %9 and range %10").arg(encoder.stats().encoderName).arg(patch.y, 0, 'f', 0).arg(patch.u, 0, 'f', 0).arg(patch.v, 0, 'f', 0).arg(decoded.middle.y).arg(decoded.middle.u).arg(decoded.middle.v).arg(sent).arg(decoded.colorspace).arg(decoded.range);
    QVERIFY(distance(decoded.middle, patch) < 6);
    QVERIFY(decoded.range != AVCOL_RANGE_JPEG);
    // VP8 has no way to say, and is BT.601 by definition. This picture is smaller than the 720
    // lines from which a stream that names nothing is taken for BT.709, so BT.709 has to be
    // named; BT.601, which goes by two names, may be left to be assumed.
    if (encoderCase.codec == VideoCodec::VP8)
        return;
    if (sent == AVCOL_SPC_BT709)
        QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
    else
        QVERIFY(decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG || decoded.colorspace == AVCOL_SPC_UNSPECIFIED);
}

// Not a check of much: a record of what a viewer is sent when a picture's size is not one the
// encoder codes. A Radeon of the 7000 series codes AV1 in blocks of 64 by 16, with two rows
// more where eight are missing, as at 1080 and 360 lines. What fills the last blocks is sent
// as picture, and is black. An AV1 frame can say at what size it is to be shown, which a
// viewer may take for the part to show, and these do not say it.
void TestVideoEncode::whatAnEncoderMakesOfASizeItCannotCode()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 5;

    QStringList seen;
    for (const QSize &size : { QSize(1280, 720), QSize(1920, 1080), QSize(640, 360), QSize(960, 540), QSize(1000, 700), QSize(2560, 1440), QSize(1452, 752) }) {
        EncodeRun run(encoderCase, size.width(), size.height(), 3'000'000);
        for (int index = 0; index < PICTURES; index++)
            QVERIFY(run.encodeNext());
        // How far what lies beyond the picture is from the picture's own last row and column.
        // The mean luma of what was decoded beyond the picture, where a row of the picture
        // itself is 128 or so: below it, and to its right.
        double below = 0;
        double beside = 0;
        const Decoded decoded = decodeAll(encoderCase.codec, run.frames, [&](const AVFrame *picture, int) {
            if (picture->format != AV_PIX_FMT_YUV420P || picture->width < size.width() || picture->height < size.height())
                return;
            double rows = 0;
            double columns = 0;
            for (int y = size.height(); y < picture->height; y++) {
                for (int x = 0; x < picture->width; x++)
                    rows += picture->data[0][y * picture->linesize[0] + x];
            }
            for (int y = 0; y < size.height(); y++) {
                for (int x = size.width(); x < picture->width; x++)
                    columns += picture->data[0][y * picture->linesize[0] + x];
            }
            below = picture->height > size.height() ? rows / ((picture->height - size.height()) * picture->width) : 0;
            beside = picture->width > size.width() ? columns / ((picture->width - size.width()) * size.height()) : 0;
        });
        QCOMPARE(decoded.errors, quint64(0));
        QCOMPARE(decoded.pictures, static_cast<int>(run.frames.size()));
        QString outcome = QStringLiteral("%1x%2 as %3x%4").arg(size.width()).arg(size.height()).arg(decoded.width).arg(decoded.height);
        if (QSize(decoded.width, decoded.height) != size)
            outcome += QStringLiteral(" (samples of %1:%2; luma of %3 below the picture and of %4 beside it)").arg(decoded.shape.num).arg(decoded.shape.den).arg(below, 0, 'f', 0).arg(beside, 0, 'f', 0);
        seen.append(outcome);
        QVERIFY2(isThePictureSent(encoderCase.codec, decoded, size), qPrintable(outcome));
        // What is offered to the server for a size rests on this being known beforehand.
        QVERIFY2(QSize(decoded.width, decoded.height) == Core::Media::codedSize(encoderCase.encoderName, size), qPrintable(outcome));
    }
    qInfo().noquote() << encoderCase.encoderName << "decodes" << seen.join(QStringLiteral(", "));
}

void TestVideoEncode::whatTheEncodersMakeOfTheSameRate_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    QTest::addColumn<bool>("page");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            for (int onTheCard : { 0, 1 }) {
                const QString from = onTheCard ? QStringLiteral(", on the card") : QStringLiteral(", from memory");
                QTest::newRow(qPrintable(name + QStringLiteral(", a moving picture") + from)) << EncoderCase{ codec, name, onTheCard } << false;
                QTest::newRow(qPrintable(name + QStringLiteral(", a page of text") + from)) << EncoderCase{ codec, name, onTheCard } << true;
            }
        }
    }
}

// Every encoder on the same pictures at the same rate, for the table cleanupTestCase prints:
// what each made of the rate, how close its pictures are to the ones it was given, and how
// long it took over one. The moving picture is tuned as a camera's is and the page as a
// screen's, which is how each would be sent.
void TestVideoEncode::whatTheEncodersMakeOfTheSameRate()
{
    QFETCH(EncoderCase, encoderCase);
    QFETCH(bool, page);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 120;
    constexpr int SETTLING = 30;
    constexpr int RATE = 1'500'000;
    const QSize size(1280, 720);
    const bool onTheCard = encoderCase.detail == 1;
    const Painter painter = page ? paintPage : paintYuv;

    VideoEncoder::Settings settings = settingsFor(encoderCase, RATE);
    settings.content = page ? VideoEncoder::Content::Screen : VideoEncoder::Content::Camera;
    VideoEncoder encoder(settings);
    if (onTheCard && !encoder.takesCardFrames())
        QSKIP("this encoder takes no pictures from the graphics card");

    AVFrame *inMemory = allocatePicture(encoder.softwareFormat(), size.width(), size.height());
    const auto freeInMemory = qScopeGuard([&inMemory] { av_frame_free(&inMemory); });
#ifdef Q_OS_WIN
    std::optional<CardPictures> card;
    if (onTheCard) {
        card.emplace(AV_PIX_FMT_NV12, size.width(), size.height(), AVCOL_SPC_BT709);
        if (!card->usable())
            QSKIP("no Direct3D 11 device takes these pictures");
    }
#else
    if (onTheCard)
        QSKIP("pictures stay on the graphics card only on Windows");
#endif

    std::vector<EncodedVideoFrame> frames;
    qint64 encodingNs = 0;
    for (int index = 0; index < PICTURES; index++) {
        AVFrame *picture = inMemory;
#ifdef Q_OS_WIN
        if (card)
            picture = card->next(index, painter);
#endif
        if (picture == inMemory) {
            av_frame_make_writable(inMemory);
            painter(inMemory, index);
        }
        QVERIFY(picture);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
        encodingNs += index > 0 ? timer.nsecsElapsed() : 0;
    }
    QVERIFY(!frames.empty() && frames[0].keyframe);
    QCOMPARE(encoder.stats().opened, quint64(1));
    QCOMPARE(encoder.stats().encoderName.contains(QStringLiteral("d3d11va")), onTheCard);
    for (const EncodedVideoFrame &frame : frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));

    size_t settledBytes = 0;
    for (const EncodedVideoFrame &frame : frames)
        settledBytes += frame.rtpTimestamp >= uint32_t(SETTLING * TICKS_PER_FRAME) ? frame.data.size() : 0;
    const double rate = static_cast<double>(settledBytes) * 8.0 * FRAMERATE / (PICTURES - SETTLING);

    Likeness likeness(painter, size, SETTLING);
    const Decoded decoded = decodeAll(encoderCase.codec, frames, [&](const AVFrame *picture, int frame) { likeness.compare(picture, static_cast<int>(frames[static_cast<size_t>(frame)].rtpTimestamp / TICKS_PER_FRAME)); });
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames.size()));
    QVERIFY(isThePictureSent(encoderCase.codec, decoded, size));

    const double msAPicture = static_cast<double>(encodingNs) / 1e6 / (PICTURES - 1);
    comparison.append(QStringLiteral("%1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10")
                              .arg(videoCodecName(encoderCase.codec), -4)
                              .arg(encoderCase.encoderName, -16)
                              .arg(page ? QStringLiteral("page of text") : QStringLiteral("moving picture"), -14)
                              .arg(onTheCard ? QStringLiteral("card") : QStringLiteral("memory"), -6)
                              .arg(RATE / 1000, 5)
                              .arg(qRound(rate / 1000), 4)
                              .arg(likeness.psnr(), 5, 'f', 2)
                              .arg(msAPicture, 5, 'f', 2)
                              .arg(frames.size(), 6)
                              .arg(frames[0].data.size(), 9));
    qInfo().noquote() << comparison.last();

    // A graphics card's encoder has a frame for every picture. libvpx is set, as libwebrtc sets
    // it, to leave pictures out when its buffer runs low, and after the key frame of a page of
    // text it leaves out most. A rate control that works is not above what it was asked for,
    // and uses it up on pictures that are hard enough; and a picture that looks nothing like
    // what went in is no encode.
    if (encoder.stats().hardware)
        QCOMPARE(frames.size(), size_t(PICTURES));
    QVERIFY2(rate < RATE * 1.3, qPrintable(QString::number(rate)));
    if (!page)
        QVERIFY2(rate > RATE * 0.7, qPrintable(QString::number(rate)));
    QVERIFY2(likeness.psnr() > 20, qPrintable(QString::number(likeness.psnr())));
}

void TestVideoEncode::cleanupTestCase()
{
    if (comparison.isEmpty())
        return;
    const QString heading = QStringLiteral("1280x720 at 30 pictures a second, measured over the last 90 of 120; PSNR is of the luma\ncodec | encoder | pictures | from | kbps asked | got | PSNR dB | ms a picture | frames | key frame bytes\n");
    qInfo().noquote() << heading + comparison.join(QLatin1Char('\n'));
}

void TestVideoEncode::picturesOnTheGraphicsCardAreEncodedThere_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            QTest::newRow(qPrintable(name + QStringLiteral(", NV12"))) << EncoderCase{ codec, name, AV_PIX_FMT_NV12 };
            QTest::newRow(qPrintable(name + QStringLiteral(", BGRA"))) << EncoderCase{ codec, name, AV_PIX_FMT_BGRA };
        }
    }
}

void TestVideoEncode::picturesOnTheGraphicsCardAreEncodedThere()
{
#ifdef Q_OS_WIN
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 40;
    constexpr int FORCED = 20;
    const bool rgb = encoderCase.detail == AV_PIX_FMT_BGRA;

    {
        // RGB pictures are converted by the encoder, to the BT.709 they are marked with.
        CardPictures pictures(static_cast<AVPixelFormat>(encoderCase.detail), WIDTH, HEIGHT, AVCOL_SPC_BT709);
        if (!pictures.usable())
            QSKIP("no Direct3D 11 device takes these pictures");

        VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
        AVFrame *fromMemory = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
        const auto freeFromMemory = qScopeGuard([&fromMemory] { av_frame_free(&fromMemory); });
        paint(fromMemory, 0);
        std::vector<EncodedVideoFrame> frames;

        if (!(rgb ? encoder.takesRgbCardFrames() : encoder.takesCardFrames())) {
            // Turned down, and nothing held against the encoder: it goes on from memory.
            AVFrame *picture = pictures.next(0);
            QVERIFY(picture);
            QVERIFY(encoder.encode(picture, 0, frames) == VideoEncoder::Outcome::CardFramesRefused);
            QVERIFY(frames.empty());
            QVERIFY(encoder.encode(fromMemory, TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
            QCOMPARE(frames.size(), size_t(1));
            QVERIFY(frames[0].keyframe);
            return;
        }

        for (int index = 0; index < PICTURES; index++) {
            if (index == FORCED)
                encoder.requestKeyframe();
            AVFrame *picture = pictures.next(index);
            QVERIFY(picture);
            QVERIFY2(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded, qPrintable(QStringLiteral("picture %1 was not taken from the card").arg(index)));
            QCOMPARE(frames.size(), size_t(index + 1));
            QCOMPARE(frames.back().rtpTimestamp, uint32_t(index * TICKS_PER_FRAME));
        }
        qInfo() << "encoder:" << encoder.stats().encoderName << (rgb ? "BGRA" : "NV12") << "pictures," << encoder.stats().averageEncodeMs << "ms a picture";
        QVERIFY(encoder.stats().encoderName.contains(QStringLiteral("d3d11va")));
        QCOMPARE(encoder.stats().opened, quint64(1));
        for (int index = 0; index < PICTURES; index++) {
            QCOMPARE(frames[static_cast<size_t>(index)].keyframe, index == 0 || index == FORCED);
            QVERIFY(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
        }

        const Decoded decoded = decodeAll(encoderCase.codec, frames);
        QCOMPARE(decoded.errors, quint64(0));
        QCOMPARE(decoded.pictures, PICTURES);
        QVERIFY(isThePictureSent(encoderCase.codec, decoded, QSize(WIDTH, HEIGHT)));
        if (rgb) {
            // And the stream says so, or a small picture would be shown as BT.601.
            qInfo() << "the patch decodes to" << decoded.middle.y << decoded.middle.u << decoded.middle.v << ", off BT.709 by" << distance(decoded.middle, patchInYuv(true)) << "and off BT.601 by" << distance(decoded.middle, patchInYuv(false));
            QVERIFY(distance(decoded.middle, patchInYuv(true)) < 6);
            QVERIFY(distance(decoded.middle, patchInYuv(false)) > 12);
            QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
        }

        // The same encoder is then given pictures from memory, as when the capture has to
        // fall back, and starts a new stream for them.
        QVERIFY(encoder.encode(fromMemory, PICTURES * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
        QCOMPARE(frames.size(), size_t(PICTURES + 1));
        QVERIFY(frames.back().keyframe);
        QVERIFY(!encoder.stats().encoderName.contains(QStringLiteral("d3d11va")));
    }
#else
    QSKIP("pictures stay on the graphics card only on Windows");
#endif
}

// Not a check of this code: a record of why ffmpeg's scale_d3d11 does or does not work here.
// It asks for a pool of ten NV12 textures in one array that are render targets and encoder
// inputs at once.
void TestVideoEncode::whichTexturesTheCardMakes()
{
#ifdef Q_OS_WIN
    struct Shape
    {
        const char *name;
        int poolSize;
        unsigned bindFlags;
    };
    const std::array shapes = {
        Shape{ "an array of ten, render target and encoder input (what scale_d3d11 asks for)", 10, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER },
        Shape{ "an array of ten, render target", 10, D3D11_BIND_RENDER_TARGET },
        Shape{ "single, render target and encoder input", 0, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER },
        Shape{ "single, render target", 0, D3D11_BIND_RENDER_TARGET },
    };
    for (const Shape &shape : shapes)
        qInfo().noquote() << "NV12 textures," << shape.name << ":" << (cardMakesNv12Textures(shape.poolSize, shape.bindFlags) ? "made" : "refused");
#else
    QSKIP("Direct3D 11 is on Windows");
#endif
}

void TestVideoEncode::theCardScalesAndConvertsPictures_data()
{
    QTest::addColumn<bool>("bt601");
    QTest::newRow("to BT.709") << false;
    QTest::newRow("to BT.601") << true;
}

// What the card makes of full-range RGB has to be the video-range YUV of the coefficients asked
// for: Y = 16 + 219 (Kr R + Kg G + Kb B) / 255, U = 128 + 224 (B - Y') / (2 (1 - Kb)) / 255 and
// V likewise from R, with Kr and Kb 0.2126 and 0.0722 for BT.709 and 0.299 and 0.114 for BT.601.
void TestVideoEncode::theCardScalesAndConvertsPictures()
{
#ifdef Q_OS_WIN
    QFETCH(bool, bt601);
    using Core::Video::CardScaler;
    // A count of rounding in each of the three, where the two sets of coefficients put the
    // colours six counts and more apart.
    constexpr double CLOSE = 2.5;
    constexpr double FAR_OFF = 5;
    const QSize size(480, 360);
    const QRect all(QPoint(0, 0), size);
    const CardScaler::Output output = { size, bt601, false };

    AVBufferRef *device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
        QSKIP("this machine has no Direct3D 11 device");
    AVFrame *large = quartersOnCard(device, QSize(1280, 960));
    AVFrame *lesser = quartersOnCard(device, QSize(640, 480));
    AVFrame *scaled = av_frame_alloc();
    AVFrame *inMemory = av_frame_alloc();
    const auto release = qScopeGuard([&] {
        av_frame_free(&inMemory);
        av_frame_free(&scaled);
        av_frame_free(&lesser);
        av_frame_free(&large);
        av_buffer_unref(&device);
    });
    QVERIFY(large && lesser);

    CardScaler scaler;
    if (!scaler.scale(large, output, scaled)) {
        QVERIFY(!CardScaler::usable());
        QSKIP("this machine's graphics card does not scale pictures");
    }
    QVERIFY(CardScaler::usable());

    // The colour found at a point given in thousandths of the picture, against the quarter
    // colour expected there, or black for none.
    const auto shows = [&](int perMilleX, int perMilleY, int quarter, const char *where) {
        const int x = size.width() * perMilleX / 1000;
        const int y = size.height() * perMilleY / 1000;
        const Yuv seen = colourAt(inMemory, x, y);
        const Yuv expected = quarter < 0 ? Yuv{ 16, 128, 128 } : inYuv(QUARTER_COLOURS[static_cast<size_t>(quarter)], !bt601);
        const double off = distance(seen, expected);
        const double offTheOther = quarter < 0 ? 0 : distance(seen, inYuv(QUARTER_COLOURS[static_cast<size_t>(quarter)], bt601));
        qInfo().noquote() << QStringLiteral("%1: %2 %3 %4, expected %5 %6 %7, off by %8 (off the other coefficients by %9)").arg(QLatin1String(where)).arg(seen.y).arg(seen.u).arg(seen.v).arg(expected.y, 0, 'f', 1).arg(expected.u, 0, 'f', 1).arg(expected.v, 0, 'f', 1).arg(off, 0, 'f', 1).arg(offTheOther, 0, 'f', 1);
        // Grey is the same under both.
        return off < CLOSE && (quarter < 0 || quarter == 3 || offTheOther > FAR_OFF);
    };
    const auto takenOffTheCard = [&] {
        av_frame_unref(inMemory);
        return av_hwframe_transfer_data(inMemory, scaled, 0) >= 0 && inMemory->format == AV_PIX_FMT_NV12 && QSize(inMemory->width, inMemory->height) == size;
    };

    // An NV12 picture on the same card, of the size asked for, that says what colours it has.
    QCOMPARE(scaled->format, static_cast<int>(AV_PIX_FMT_D3D11));
    QCOMPARE(formatOnCard(scaled), AV_PIX_FMT_NV12);
    QCOMPARE(QSize(scaled->width, scaled->height), size);
    QVERIFY(reinterpret_cast<const AVHWFramesContext *>(scaled->hw_frames_ctx->data)->device_ref->data == device->data);
    QCOMPARE(static_cast<int>(scaled->colorspace), static_cast<int>(bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709));
    QCOMPARE(static_cast<int>(scaled->color_range), static_cast<int>(AVCOL_RANGE_MPEG));
    const uint8_t *const kind = scaled->hw_frames_ctx->data;

    // Each quarter where it was, in its colour, whether or not the source had any opacity.
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(250, 250, 0, "the whole picture, top left"));
    QVERIFY(shows(750, 250, 1, "the whole picture, top right"));
    QVERIFY(shows(250, 750, 2, "the whole picture, bottom left"));
    QVERIFY(shows(750, 750, 3, "the whole picture, bottom right"));

    // A part of the source is all that is drawn: its right half, and its bottom left quarter.
    QVERIFY(scaler.scale(large, QRect(640, 0, 640, 960), output, all, scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(100, 250, 1, "the right half, top left"));
    QVERIFY(shows(900, 250, 1, "the right half, top right"));
    QVERIFY(shows(100, 750, 3, "the right half, bottom left"));
    QVERIFY(shows(900, 750, 3, "the right half, bottom right"));
    QVERIFY(scaler.scale(large, QRect(0, 480, 640, 480), output, all, scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(100, 100, 2, "the bottom left quarter, top left"));
    QVERIFY(shows(900, 900, 2, "the bottom left quarter, bottom right"));

    // Drawn over a part of the new picture, the rest of which is black.
    QVERIFY(scaler.scale(large, QRect(0, 0, 1280, 960), output, QRect(120, 90, 240, 180), scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(375, 375, 0, "placed in the middle, its top left"));
    QVERIFY(shows(625, 625, 3, "placed in the middle, its bottom right"));
    QVERIFY(shows(100, 100, -1, "placed in the middle, outside it at the top left"));
    QVERIFY(shows(900, 500, -1, "placed in the middle, outside it on the right"));
    QVERIFY(scaled->hw_frames_ctx->data == kind);

    // A source of another size makes pictures of the same kind, so an encoder goes on with them.
    QVERIFY(scaler.scale(lesser, output, scaled));
    QVERIFY(scaled->hw_frames_ctx->data == kind);
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(250, 250, 0, "the smaller source, top left"));
    QVERIFY(shows(750, 750, 3, "the smaller source, bottom right"));
    QVERIFY(scaler.scale(large, output, scaled));
    QVERIFY(scaled->hw_frames_ctx->data == kind);

    constexpr int PICTURES = 120;
    const double before = processCpuMs();
    for (int index = 0; index < PICTURES; index++)
        QVERIFY(scaler.scale(index % 2 ? lesser : large, output, scaled));
    qInfo().noquote() << QStringLiteral("scaling on the card: %1 ms of processor time a picture").arg((processCpuMs() - before) / PICTURES, 0, 'f', 3);
#else
    QSKIP("Direct3D 11 is on Windows");
#endif
}

void TestVideoEncode::picturesScaledOnTheCardAreEncodedThere_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec))
            QTest::newRow(qPrintable(name)) << EncoderCase{ codec, name };
    }
}

void TestVideoEncode::picturesScaledOnTheCardAreEncodedThere()
{
#ifdef Q_OS_WIN
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    using Core::Video::CardScaler;
    constexpr int PICTURES = 40;
    const Core::Video::CardScaler::Output output = { QSize(WIDTH, HEIGHT), false, false };

    VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
    if (!encoder.takesCardFrames())
        QSKIP("this encoder takes no pictures from the graphics card");
    CardPictures pictures(AV_PIX_FMT_BGRA, 2 * WIDTH, 2 * HEIGHT, AVCOL_SPC_RGB);
    if (!pictures.usable())
        QSKIP("no Direct3D 11 device takes these pictures");

    CardScaler scaler;
    AVFrame *scaled = av_frame_alloc();
    const auto freeScaled = qScopeGuard([&scaled] { av_frame_free(&scaled); });
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        AVFrame *picture = pictures.next(index);
        QVERIFY(picture);
        if (!scaler.scale(picture, output, scaled)) {
            QVERIFY(!CardScaler::usable());
            QSKIP("this machine's graphics card does not scale pictures");
        }
        QCOMPARE(formatOnCard(scaled), AV_PIX_FMT_NV12);
        QVERIFY2(encoder.encode(scaled, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded, qPrintable(QStringLiteral("picture %1 was not taken from the card").arg(index)));
        QCOMPARE(frames.size(), size_t(index + 1));
    }
    qInfo() << "encoder:" << encoder.stats().encoderName << encoder.stats().averageEncodeMs << "ms a picture";
    QVERIFY(encoder.stats().encoderName.contains(QStringLiteral("d3d11va")));
    QCOMPARE(encoder.stats().opened, quint64(1));
    for (int index = 0; index < PICTURES; index++) {
        QCOMPARE(frames[static_cast<size_t>(index)].keyframe, index == 0);
        QVERIFY(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
    }

    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, PICTURES);
    QVERIFY(isThePictureSent(encoderCase.codec, decoded, output.size));
    qInfo() << "the patch decodes to" << decoded.middle.y << decoded.middle.u << decoded.middle.v << ", off BT.709 by" << distance(decoded.middle, patchInYuv(true)) << "and off BT.601 by" << distance(decoded.middle, patchInYuv(false)) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY(distance(decoded.middle, patchInYuv(true)) < 6);
    QVERIFY(distance(decoded.middle, patchInYuv(false)) > 12);
    QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
#else
    QSKIP("pictures stay on the graphics card only on Windows");
#endif
}

void TestVideoEncode::screensAreListedWithThumbnails()
{
    if (!Core::Video::screenCaptureAvailable()) {
        QVERIFY(Core::Video::captureSources().isEmpty());
        QSKIP("this build or this machine cannot capture a screen");
    }
    const QList<Core::Video::CaptureSource> sources = Core::Video::captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");

    const QSize bound(320, 180);
    for (int i = 0; i < sources.size(); i++) {
        QCOMPARE(sources[i].id, QStringLiteral("screen:%1").arg(i));
        QVERIFY(!sources[i].name.isEmpty());
        QVERIFY(sources[i].geometry.width() >= 320 && sources[i].geometry.height() >= 200);

        QElapsedTimer timer;
        timer.start();
        const QImage thumbnail = Core::Video::captureThumbnail(sources[i], bound);
        qInfo() << sources[i].id << sources[i].name << sources[i].geometry << "thumbnail" << thumbnail.size() << "in" << timer.elapsed() << "ms";
        QVERIFY(!thumbnail.isNull());
        QCOMPARE(thumbnail.size(), sources[i].geometry.size().scaled(bound, Qt::KeepAspectRatio));
        QCOMPARE(thumbnail.format(), QImage::Format_RGB32);
        QCOMPARE(*reinterpret_cast<const quint32 *>(thumbnail.constScanLine(0)) >> 24, 0xFFu);
    }
    QVERIFY(Core::Video::captureThumbnail({}, bound).isNull());
}

void TestVideoEncode::thePipelineSharesTheScreen_data()
{
    using Core::Video::CaptureMethod;
    QTest::addColumn<EncoderCase>("encoderCase");
    QTest::addColumn<CaptureMethod>("method");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : encodable) {
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at 720p"))) << EncoderCase{ codec, {}, 720 } << CaptureMethod::Auto;
        // Nothing to scale, which is when the pictures can go to the encoder as captured.
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at the screen's own size"))) << EncoderCase{ codec, {}, 0 } << CaptureMethod::Auto;
    }
    if (!encodable.isEmpty()) {
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at 720p, by graphics capture"))) << EncoderCase{ encodable.first(), {}, 720 } << CaptureMethod::GraphicsCapture;
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at the screen's own size, by graphics capture"))) << EncoderCase{ encodable.first(), {}, 0 } << CaptureMethod::GraphicsCapture;
        // The second screen again, for what is in the middle of it.
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at 720p, a second screen by duplication"))) << EncoderCase{ encodable.first(), {}, 720 } << CaptureMethod::DesktopDuplication;
    }
}

void TestVideoEncode::thePipelineSharesTheScreen()
{
    if (Core::Media::encodableVideoCodecs().isEmpty())
        QSKIP("this machine has no encoder");
    QFETCH(EncoderCase, encoderCase);
    QFETCH(Core::Video::CaptureMethod, method);
    using namespace Core::Video;
    if (!screenCaptureAvailable())
        QSKIP("this build or this machine cannot capture a screen");
    if (!captureMethodUsable(method))
        QSKIP("this build or this Windows does not have the capture method");
    const QList<CaptureSource> sources = captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");
    if (!aScreenIsOnAGraphicsCard())
        QSKIP("no screen of this machine is on a graphics card");
    setCaptureMethod(method);
    CaptureSource source = sources.first();
#ifdef Q_OS_WIN
    // Graphics capture has a picture only when the screen changes, which a window of the test's
    // own sees to. That goes on a second screen, and that screen is then the one shared. It is
    // over the middle of its screen, so the middle of the picture is its colour.
    std::unique_ptr<TestWindow> stirring;
    if (method != CaptureMethod::Auto) {
        SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
        const auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
        QVERIFY(shared != sources.end());
        source = *shared;
        stirring = std::make_unique<TestWindow>(TestWindow::Look{ QSize(440, 330), quiet->whole.center() - QPoint(220, 190) });
        QVERIFY(stirring->isOpen());
        QVERIFY(stirring->insideOnScreen().adjusted(40, 40, -40, -40).contains(quiet->whole.center()));
    }
#endif

    // A screen of a size the codec is not offered for is not shared in it. A row that is about
    // how the screen is captured goes on in the codec such a screen is shared in.
    const QSize expectedSize = encodeSizeFor(source.geometry.size(), encoderCase.detail);
    const QList<VideoCodec> shared = codecsSharedAt(expectedSize);
    if (!shared.contains(encoderCase.codec) && method == CaptureMethod::Auto)
        QSKIP("a screen of this size is not shared in this codec, whose encoder would add black to it");
    const VideoCodec codec = shared.contains(encoderCase.codec) ? encoderCase.codec : shared.first();

    BroadcastPipeline pipeline;
    QObject here;
    std::vector<EncodedFramePointer> frames;
    QList<QSize> starts;
    QList<QImage> previews;
    QList<BroadcastStats> stats;
    QList<bool> stalls;
    QStringList failures;
    connect(&pipeline, &BroadcastPipeline::started, &here, [&](const QSize &size, int framerate) {
        QCOMPARE(framerate, FRAMERATE);
        starts.append(size);
    });
    connect(&pipeline, &BroadcastPipeline::frameEncoded, &here, [&](const EncodedFramePointer &frame) { frames.push_back(frame); });
    connect(&pipeline, &BroadcastPipeline::previewReady, &here, [&](const QImage &preview) { previews.append(preview); });
    connect(&pipeline, &BroadcastPipeline::statsUpdated, &here, [&](const BroadcastStats &update) { stats.append(update); });
    connect(&pipeline, &BroadcastPipeline::captureStalled, &here, [&](bool stalled) { stalls.append(stalled); });
    connect(&pipeline, &BroadcastPipeline::failed, &here, [&](const QString &reason) { failures.append(reason); });

    BroadcastSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = encoderCase.detail;
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrate = 2'500'000;
    settings.maxBitrate = 4'000'000;

    QElapsedTimer sinceStart;
    sinceStart.start();
    pipeline.start(settings);
    // Long enough for a machine that draws its screen in software.
    QTRY_VERIFY2_WITH_TIMEOUT(frames.size() >= 20 || !failures.isEmpty(), "fewer than 20 frames in 10 s", 10000);
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char(';'))));
    qInfo() << "20 frames after" << sinceStart.elapsed() << "ms";

    QCOMPARE(starts.size(), 1);
    QCOMPARE(starts.first(), expectedSize);
    QVERIFY(frames[0]->keyframe);
    QVERIFY(!frames[1]->keyframe);

    // Time stamps are when each picture was captured, on a 90 kHz clock.
    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(frames[i]->rtpTimestamp - frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(frames[19]->rtpTimestamp - frames[0]->rtpTimestamp) / 19.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8 && ticksPerFrame < TICKS_PER_FRAME * 1.25, qPrintable(QString::number(ticksPerFrame)));

    // A key frame comes with the next picture or two after it is asked for: the pictures
    // already on their way here when the request went out are the allowance.
    const size_t atRequest = frames.size();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= atRequest + 6, 1000);
    const auto forced = std::find_if(frames.begin() + static_cast<std::ptrdiff_t>(atRequest), frames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; });
    QVERIFY(forced != frames.end());
    QVERIFY(forced - frames.begin() < static_cast<std::ptrdiff_t>(atRequest) + 4);

    // What sharing costs this process while the test itself sits still.
    const double cpuBefore = processCpuMs();
    const size_t framesBefore = frames.size();
    QTest::qWait(1000);
    const double cpuPerFrame = (processCpuMs() - cpuBefore) / static_cast<double>(std::max<size_t>(1, frames.size() - framesBefore));

    // Nobody watching: nothing is encoded, and the previews carry on.
    QTRY_VERIFY_WITH_TIMEOUT(!previews.isEmpty(), 1000);
    pipeline.setEncodingSuspended(true);
    QTest::qWait(300);
    const size_t whileSuspended = frames.size();
    const int previewsBefore = previews.size();
    QTest::qWait(1200);
    QCOMPARE(frames.size(), whileSuspended);
    QVERIFY(previews.size() >= previewsBefore + 2);

    pipeline.setEncodingSuspended(false);
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= whileSuspended + 10, 1500);
    QVERIFY(frames[whileSuspended]->keyframe);
    QVERIFY(!frames[whileSuspended + 1]->keyframe);

    // A rate the encoder is moved to while it runs.
    pipeline.setBitrate(1'000'000);
    const size_t atNewRate = frames.size();
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= atNewRate + 15, 1500);

    for (const QImage &preview : previews) {
        QVERIFY(!preview.isNull());
        QVERIFY(preview.width() <= 512 && preview.height() <= 288);
        QCOMPARE(preview.size(), expectedSize.scaled(QSize(512, 288), Qt::KeepAspectRatio));
    }

    QTRY_VERIFY_WITH_TIMEOUT(stats.size() >= 2, 2500);
    const BroadcastStats lastStats = stats.last();
    qInfo().noquote() << QStringLiteral("pipeline: %1, hardware %2, %3x%4, captured %5 fps, %6 ms in the encoder a picture, %7 ms of processor time a picture in all, %8 frames, %9 dropped").arg(lastStats.encoderName).arg(lastStats.hardware).arg(lastStats.encodedSize.width()).arg(lastStats.encodedSize.height()).arg(lastStats.capturedFps, 0, 'f', 1).arg(lastStats.averageEncodeMs, 0, 'f', 2).arg(cpuPerFrame, 0, 'f', 2).arg(lastStats.framesEncoded).arg(lastStats.framesDropped);
    QVERIFY(lastStats.encoderName.endsWith(method == CaptureMethod::GraphicsCapture ? QStringLiteral(", from gfxcapture") : QStringLiteral(", from ddagrab")));
    QCOMPARE(lastStats.encodedSize, expectedSize);
    QVERIFY(lastStats.capturedFps > FRAMERATE * 0.8 && lastStats.capturedFps < FRAMERATE * 1.2);
    // Scaled or not, the pictures never leave the card on their way to an encoder that is on it.
    QCOMPARE(lastStats.encoderName.contains(QStringLiteral("(d3d11va)")), reachesTheEncoderOnTheCard(codec, expectedSize != source.geometry.size(), method == CaptureMethod::GraphicsCapture));

    pipeline.stop();
    QTest::qWait(300);
    const size_t afterStop = frames.size();
    QTest::qWait(300);
    QCOMPARE(frames.size(), afterStop);
    QVERIFY(failures.isEmpty());
    QVERIFY(stalls.isEmpty());

    // Everything sent decodes as one stream, the gap and the new rate included.
    for (const EncodedFramePointer &frame : frames)
        QVERIFY(isWellFormed(codec, *frame));
    const Decoded decoded = decodeAll(codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames.size()));
    QVERIFY2(isThePictureSent(codec, decoded, expectedSize), qPrintable(QStringLiteral("%1x%2").arg(decoded.width).arg(decoded.height)));
#ifdef Q_OS_WIN
    if (stirring) {
        qInfo() << "the middle of the screen decodes off the window's colour by" << offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
        QVERIFY2(offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window in the middle of the screen is not its colour in the picture");
    }
#endif

    // The same pipeline shares again, and time does not run backwards across the restart.
    const uint32_t lastTimestamp = frames.back()->rtpTimestamp;
    frames.clear();
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(frames.size() >= 5 || !failures.isEmpty(), "no frames after a restart", 3000);
    QVERIFY(failures.isEmpty());
    QCOMPARE(starts.size(), 2);
    QVERIFY(frames[0]->keyframe);
    QVERIFY(static_cast<int32_t>(frames[0]->rtpTimestamp - lastTimestamp) > 0);
}

void TestVideoEncode::whatScalingAScreenCosts_data()
{
    using Core::Video::CaptureMethod;
    QTest::addColumn<CaptureMethod>("method");
    QTest::newRow("by duplication") << CaptureMethod::DesktopDuplication;
    QTest::newRow("by graphics capture") << CaptureMethod::GraphicsCapture;
}

// Not a check of much: a record of what capturing a screen and scaling it to 720p costs this
// process before any encoder has it, each way a capture may be asked to hand its pictures over.
// In software is how every scaled share went before the card was made to scale.
void TestVideoEncode::whatScalingAScreenCosts()
{
#ifdef Q_OS_WIN
    QFETCH(Core::Video::CaptureMethod, method);
    using namespace Core::Video;
    constexpr int SETTLING = 15;
    constexpr int PICTURES = 60;
    constexpr int MAX_HEIGHT = 720;
    if (!screenCaptureAvailable() || !captureMethodUsable(method))
        QSKIP("this build or this Windows cannot capture a screen this way");
    const QList<CaptureSource> sources = captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");
    if (!aScreenIsOnAGraphicsCard())
        QSKIP("no screen of this machine is on a graphics card");

    // Graphics capture has a picture only when its screen changes, which a window sees to.
    CaptureSource source = sources.first();
    std::unique_ptr<TestWindow> stirring;
    if (method == CaptureMethod::GraphicsCapture) {
        SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
        const auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
        QVERIFY(shared != sources.end());
        source = *shared;
        stirring = std::make_unique<TestWindow>(TestWindow::Look{ QSize(96, 96), quiet->cornerFor(QSize(160, 180)) });
    }
    const QSize size = encodeSizeFor(source.geometry.size(), MAX_HEIGHT);
    if (size == source.geometry.size())
        QSKIP("the screen is no larger than 720p, so nothing is scaled");

    struct Way
    {
        const char *name;
        bool keepOnCard;
        bool rgbOnCard;
    };
    const std::array ways = {
        Way{ "read back and scaled in software", false, false },
        Way{ "kept on the card for an encoder that takes NV12 and the screen's own BGRA", true, true },
        Way{ "kept on the card for an encoder that takes only NV12", true, false },
    };
    // Pictures are kept on the card only for an encoder that takes them there, so where there
    // is none that is never asked of the capture, and need not work.
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    const bool anEncoderTakesCardFrames = std::any_of(encodable.begin(), encodable.end(), [](VideoCodec codec) { return VideoEncoder(settingsFor({ codec, {} }, BITRATE)).takesCardFrames(); });
    for (const Way &way : ways) {
        if (way.keepOnCard && !anEncoderTakesCardFrames)
            continue;
        ScreenCapture::Request request;
        request.sourceId = source.id;
        request.method = method;
        request.maxHeight = MAX_HEIGHT;
        request.framerate = FRAMERATE;
        request.softwareFormat = AV_PIX_FMT_NV12;
        request.keepOnCard = way.keepOnCard;
        request.rgbOnCard = way.rgbOnCard;
        ScreenCapture capture;
        capture.configure(request);

        AVFrame *picture = av_frame_alloc();
        const auto freePicture = qScopeGuard([&picture] { av_frame_free(&picture); });
        int read = 0;
        int onCard = 0;
        int nv12 = 0;
        double cpuAfterSettling = 0;
        QElapsedTimer timer;
        timer.start();
        while (read < SETTLING + PICTURES && timer.elapsed() < 10000) {
            const ScreenCapture::Status status = capture.read(picture);
            QVERIFY2(status != ScreenCapture::Status::Failed && status != ScreenCapture::Status::Closed, qPrintable(capture.failure()));
            if (status != ScreenCapture::Status::Frame)
                continue;
            QCOMPARE(QSize(picture->width, picture->height), size);
            QCOMPARE(static_cast<int>(picture->color_range), static_cast<int>(AVCOL_RANGE_MPEG));
            QCOMPARE(static_cast<int>(picture->colorspace), static_cast<int>(AVCOL_SPC_BT709));
            onCard += picture->hw_frames_ctx ? 1 : 0;
            nv12 += (picture->hw_frames_ctx ? formatOnCard(picture) : static_cast<AVPixelFormat>(picture->format)) == AV_PIX_FMT_NV12 ? 1 : 0;
            av_frame_unref(picture);
            if (++read == SETTLING)
                cpuAfterSettling = processCpuMs();
        }
        QCOMPARE(read, SETTLING + PICTURES);
        qInfo().noquote() << QStringLiteral("a %1x%2 screen captured as %3x%4, %5: %6 ms of processor time a picture; %7 of %8 pictures on the card, %9 of them NV12").arg(source.geometry.width()).arg(source.geometry.height()).arg(size.width()).arg(size.height()).arg(QLatin1String(way.name)).arg((processCpuMs() - cpuAfterSettling) / PICTURES, 0, 'f', 2).arg(onCard).arg(read).arg(nv12);

        // Graphics capture scales by itself for an encoder that takes its BGRA pictures. Every
        // other picture that stays on the card is the video processor's, and NV12.
        const bool scaledByTheSource = method == CaptureMethod::GraphicsCapture && way.rgbOnCard;
        QCOMPARE(onCard, way.keepOnCard && (scaledByTheSource || CardScaler::usable()) ? read : 0);
        QCOMPARE(nv12, scaledByTheSource ? 0 : read);
    }
#else
    QSKIP("screens are captured on Windows");
#endif
}

void TestVideoEncode::aScreenThatIsNotThereFails()
{
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");

    BroadcastPipeline pipeline;
    QObject here;
    QStringList failures;
    int framesSeen = 0;
    int starts = 0;
    connect(&pipeline, &BroadcastPipeline::failed, &here, [&](const QString &reason) { failures.append(reason); });
    connect(&pipeline, &BroadcastPipeline::frameEncoded, &here, [&](const EncodedFramePointer &) { framesSeen++; });
    connect(&pipeline, &BroadcastPipeline::started, &here, [&](const QSize &, int) { starts++; });

    BroadcastSettings settings;
    settings.sourceId = QStringLiteral("screen:99");
    settings.codec = encodable.first();
    settings.bitrate = 1'000'000;
    pipeline.start(settings);

    // Not at once: a screen may be away for a moment and come back.
    QTest::qWait(1000);
    if (screenCaptureAvailable())
        QVERIFY(failures.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, ScreenCapture::GIVE_UP_AFTER_MS + 2000);
    qInfo() << "failed with:" << failures.first();
    QVERIFY(!failures.first().isEmpty());
    QCOMPARE(framesSeen, 0);
    QCOMPARE(starts, 0);
}

void TestVideoEncode::theCaptureMethodIsRemembered()
{
    using namespace Core::Video;
    const QString key = QStringLiteral("video/capture_method");
    QCOMPARE(captureMethod(), CaptureMethod::Auto);

    setCaptureMethod(CaptureMethod::GraphicsCapture);
    QCOMPARE(captureMethod(), CaptureMethod::GraphicsCapture);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("graphics_capture"));
    setCaptureMethod(CaptureMethod::DesktopDuplication);
    QCOMPARE(captureMethod(), CaptureMethod::DesktopDuplication);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("desktop_duplication"));
    setCaptureMethod(CaptureMethod::Auto);
    QCOMPARE(captureMethod(), CaptureMethod::Auto);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("auto"));

    // Anything else, and nothing at all, is the default.
    QSettings().setValue(key, QStringLiteral("something else"));
    QCOMPARE(captureMethod(), CaptureMethod::Auto);
    QSettings().remove(key);
    QCOMPARE(captureMethod(), CaptureMethod::Auto);

    const bool byDuplication = captureMethodUsable(CaptureMethod::DesktopDuplication);
    const bool byGraphicsCapture = captureMethodUsable(CaptureMethod::GraphicsCapture);
    qInfo() << "usable here: desktop duplication" << byDuplication << ", graphics capture" << byGraphicsCapture;
    QCOMPARE(byDuplication, screenCaptureAvailable());
    QCOMPARE(captureMethodUsable(CaptureMethod::Auto), byDuplication || byGraphicsCapture);
    // A window can be shared wherever either method works, a screen included.
    QCOMPARE(windowCaptureAvailable(), byDuplication || byGraphicsCapture);
}

void TestVideoEncode::aStillPictureIsSentAgain()
{
    using Core::Video::stillPictureIsDue;
    // A couple a second while nobody is waiting for anything.
    QVERIFY(!stillPictureIsDue(0, false, 30));
    QVERIFY(!stillPictureIsDue(499, false, 30));
    QVERIFY(stillPictureIsDue(500, false, 30));
    QVERIFY(stillPictureIsDue(5000, false, 30));
    // A key frame that is waited for comes with the next picture the frame rate allows.
    QVERIFY(!stillPictureIsDue(0, true, 30));
    QVERIFY(!stillPictureIsDue(32, true, 30));
    QVERIFY(stillPictureIsDue(33, true, 30));
    QVERIFY(!stillPictureIsDue(65, true, 15));
    QVERIFY(stillPictureIsDue(66, true, 15));
    QVERIFY(stillPictureIsDue(500, true, 1));
    QVERIFY(stillPictureIsDue(1000, true, 0));
}

// The official client's rule for when a window cannot be cut out of the screen by itself.
void TestVideoEncode::aWindowAboveCoversTheOneBelow()
{
    using Core::Video::StackedWindow;
    using Core::Video::windowIsCovered;
    const auto at = [](int x, int y, int width, int height) {
        StackedWindow window;
        window.bounds = QRect(x, y, width, height);
        return window;
    };
    const StackedWindow target = at(100, 100, 400, 300);

    QVERIFY(!windowIsCovered({ target }, 0));
    QVERIFY(windowIsCovered({ at(300, 200, 400, 300), target }, 1));
    // By a single pixel as well, and by a window that lies wholly inside it.
    QVERIFY(windowIsCovered({ at(499, 399, 50, 50), target }, 1));
    QVERIFY(windowIsCovered({ at(200, 200, 10, 10), target }, 1));
    // One that only touches it, or is somewhere else, does not.
    QVERIFY(!windowIsCovered({ at(500, 100, 200, 300), target }, 1));
    QVERIFY(!windowIsCovered({ at(100, 400, 400, 50), target }, 1));
    QVERIFY(!windowIsCovered({ at(900, 900, 200, 200), at(0, 0, 100, 100), target }, 2));

    // Only what is above counts, however much of it what is below would hide.
    QVERIFY(!windowIsCovered({ target, at(0, 0, 2000, 2000) }, 0));
    QVERIFY(windowIsCovered({ at(0, 0, 2000, 2000), target, at(0, 0, 2000, 2000) }, 1));

    // What does not show on the screen covers nothing, nor does an overlay.
    for (int ignored = 0; ignored < 6; ignored++) {
        StackedWindow above = at(0, 0, 2000, 2000);
        above.visible = ignored != 0;
        above.minimised = ignored == 1;
        above.cloaked = ignored == 2;
        above.overlay = ignored == 3;
        above.tabProxy = ignored == 4;
        if (ignored == 5)
            above.bounds = QRect();
        QVERIFY2(!windowIsCovered({ above, target }, 1), qPrintable(QString::number(ignored)));
    }
    // A tool window and a window of the same program are windows like any other.
    QVERIFY(windowIsCovered({ at(0, 0, 2000, 2000), at(150, 150, 20, 20), target }, 2));

    // The taskbar's full-size preview shows over everything wherever it says it is.
    StackedWindow preview = at(5000, 5000, 10, 10);
    preview.livePreview = true;
    QVERIFY(windowIsCovered({ preview, target }, 1));
    preview.visible = false;
    QVERIFY(!windowIsCovered({ preview, target }, 1));

    // A window that shows nothing of itself cannot be cut out of the screen either.
    StackedWindow gone = target;
    gone.minimised = true;
    QVERIFY(windowIsCovered({ gone }, 0));
    gone = target;
    gone.cloaked = true;
    QVERIFY(windowIsCovered({ gone }, 0));
    gone = target;
    gone.bounds = QRect(100, 100, 0, 0);
    QVERIFY(windowIsCovered({ gone }, 0));
    QVERIFY(windowIsCovered({ target }, 1));
    QVERIFY(windowIsCovered({}, 0));
}

void TestVideoEncode::windowsAreListedWithoutThisProgramsOwn()
{
#ifdef Q_OS_WIN
    using namespace Core::Video;
    // With a window of this program's own to leave out, where there is a screen to put it on.
    std::unique_ptr<TestWindow> own;
    if (const std::optional<QuietScreen> quiet = quietScreen()) {
        own = std::make_unique<TestWindow>(TestWindow::Look{ QSize(320, 240), quiet->cornerFor(QSize(400, 340)) });
        QVERIFY(own->isOpen());
        QTest::qWait(200);
    }

    const QList<CaptureSource> windows = captureWindows();
    if (!windowCaptureAvailable()) {
        QVERIFY(windows.isEmpty());
        QSKIP("this build or this Windows cannot capture a window");
    }

    // Their titles are the user's own business and stay out of the log.
    qInfo() << windows.size() << "windows listed";
    QStringList ids;
    for (const CaptureSource &window : windows) {
        QCOMPARE(window.kind, CaptureSource::Kind::Window);
        QVERIFY(window.id.startsWith(QStringLiteral("window:")));
        QVERIFY(!ids.contains(window.id));
        ids.append(window.id);
        QVERIFY(!window.name.isEmpty());
        QVERIFY(window.processId != 0);
        QVERIFY(window.processId != GetCurrentProcessId());

        bool numbered = false;
        const HWND handle = reinterpret_cast<HWND>(static_cast<quintptr>(window.id.mid(7).toULongLong(&numbered)));
        QVERIFY(numbered);
        // One may have been closed or minimised since it was listed.
        if (IsWindow(handle) && !IsIconic(handle) && IsWindowVisible(handle))
            QVERIFY(window.geometry.width() > 0 && window.geometry.height() > 0);
    }
    if (own)
        QVERIFY(!ids.contains(own->sourceId()));
#else
    QVERIFY(Core::Video::captureWindows().isEmpty());
#endif
}

void TestVideoEncode::aWindowHasAThumbnail_data()
{
    QTest::addColumn<bool>("scaledByWindows");
    QTest::newRow("a window that measures in the screen's pixels") << false;
    QTest::newRow("a window Windows scales") << true;
}

void TestVideoEncode::aWindowHasAThumbnail()
{
#ifdef Q_OS_WIN
    QFETCH(bool, scaledByWindows);
    using namespace Core::Video;
    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const TestWindow window(TestWindow::Look{ QSize(480, 360), quiet->cornerFor(QSize(560, 460)), SHARED_COLOUR, true, scaledByWindows });
    QVERIFY(window.isOpen());
    QTest::qWait(300);

    CaptureSource source;
    source.id = window.sourceId();
    source.kind = CaptureSource::Kind::Window;
    const QSize bound(224, 126);
    QElapsedTimer timer;
    timer.start();
    const QImage thumbnail = captureThumbnail(source, bound);
    qInfo() << "thumbnail" << thumbnail.size() << "in" << timer.elapsed() << "ms";
    QVERIFY(!thumbnail.isNull());
    QVERIFY(thumbnail.width() <= bound.width() && thumbnail.height() <= bound.height());
    QVERIFY(thumbnail.width() == bound.width() || thumbnail.height() == bound.height());
    QCOMPARE(thumbnail.format(), QImage::Format_RGB32);

    // The whole window as it shows, so its inside is in the middle and at the bottom, and its
    // title bar, which is another colour, at the top.
    const auto isTheWindowsColour = [](QRgb pixel) { return qAbs(qRed(pixel) - PATCH_RED) < 12 && qAbs(qGreen(pixel) - PATCH_GREEN) < 12 && qAbs(qBlue(pixel) - PATCH_BLUE) < 12; };
    QVERIFY(isTheWindowsColour(thumbnail.pixel(thumbnail.width() / 2, thumbnail.height() / 2)));
    QVERIFY(isTheWindowsColour(thumbnail.pixel(thumbnail.width() / 2, thumbnail.height() - 4)));
    QVERIFY(isTheWindowsColour(thumbnail.pixel(thumbnail.width() - 4, thumbnail.height() / 2)));
    QVERIFY(!isTheWindowsColour(thumbnail.pixel(thumbnail.width() / 2, 2)));

    source.id = QStringLiteral("window:1");
    QVERIFY(captureThumbnail(source, bound).isNull());
#else
    QSKIP("windows are captured on Windows");
#endif
}

void TestVideoEncode::thePipelineSharesAWindow_data()
{
    using Core::Video::CaptureMethod;
    QTest::addColumn<CaptureMethod>("method");
    QTest::addColumn<bool>("scaledByWindows");
    QTest::addColumn<int>("maxHeight");
    QTest::newRow("by graphics capture") << CaptureMethod::GraphicsCapture << false << 0;
    QTest::newRow("by graphics capture, a window Windows scales") << CaptureMethod::GraphicsCapture << true << 0;
    QTest::newRow("by graphics capture, scaled down") << CaptureMethod::GraphicsCapture << false << 360;
    QTest::newRow("by duplication") << CaptureMethod::DesktopDuplication << false << 0;
    QTest::newRow("by duplication, a window Windows scales") << CaptureMethod::DesktopDuplication << true << 0;
    QTest::newRow("by duplication, scaled down") << CaptureMethod::DesktopDuplication << false << 360;
}

void TestVideoEncode::thePipelineSharesAWindow()
{
#ifdef Q_OS_WIN
    QFETCH(Core::Video::CaptureMethod, method);
    QFETCH(bool, scaledByWindows);
    QFETCH(int, maxHeight);
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (!windowCaptureAvailable() || !captureMethodUsable(method))
        QSKIP("this build or this Windows cannot capture a window this way");
    // What follows looks at where things are in the decoded picture, so it takes a codec whose
    // pictures come out at the size they went in, which AV1's do not from every graphics card.
    const VideoCodec codec = encodable.contains(VideoCodec::H264) ? VideoCodec::H264 : encodable.first();
    const bool duplicated = method == CaptureMethod::DesktopDuplication;
    // Two lots of the statistics, a second apart, for what was captured how in between.
    const auto countsOverASecond = [](PipelineRecord &record, PathCounts *earlier, PathCounts *later) {
        for (PathCounts *counts : { earlier, later }) {
            const int seen = record.stats.size();
            if (!QTest::qWaitFor([&record, seen] { return record.stats.size() > seen; }, 2500))
                return false;
            *counts = pathCounts(record.stats.last());
        }
        return true;
    };

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const QPoint home = quiet->cornerFor(QSize(860, 700));
    TestWindow window(TestWindow::Look{ QSize(640, 480), home, SHARED_COLOUR, true, scaledByWindows });
    QVERIFY(window.isOpen());
    QTest::qWait(200);
    const QSize firstSize = encodeSizeFor(window.insideOnScreen().size(), maxHeight);
    QCOMPARE(firstSize != window.insideOnScreen().size(), maxHeight != 0);

    setCaptureMethod(method);
    BroadcastPipeline pipeline;
    PipelineRecord record(pipeline);
    BroadcastSettings settings;
    settings.sourceId = window.sourceId();
    settings.maxHeight = maxHeight;
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrate = 2'500'000;
    settings.maxBitrate = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty() || record.closes > 0, "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.closes, 0);
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first(), firstSize);
    QVERIFY(record.frames[0]->keyframe);
    QVERIFY(!record.frames[1]->keyframe);

    // Time stamps only grow, and no faster than the rate asked for.
    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(record.frames[i]->rtpTimestamp - record.frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(record.frames[19]->rtpTimestamp - record.frames[4]->rtpTimestamp) / 15.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8, qPrintable(QString::number(ticksPerFrame)));

    // Where in the frames each part of what follows begins, to say of a picture that should
    // not have been sent what the window was doing when it was.
    QList<std::pair<size_t, QString>> parts;
    const auto partBegins = [&parts, &record](const QString &name) { parts.append({ record.frames.size(), name }); };
    partBegins(QStringLiteral("started"));

    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    qInfo().noquote() << "captured as" << record.stats.last().encoderName;
    if (duplicated)
        QVERIFY(pathCounts(record.stats.last()).cutOut > 0);
    else
        QVERIFY(record.stats.last().encoderName.endsWith(QStringLiteral(", from gfxcapture")));
    const bool onCard = reachesTheEncoderOnTheCard(codec, maxHeight != 0, !duplicated);
    QCOMPARE(record.stats.last().encoderName.contains(QStringLiteral("(d3d11va)")), onCard);

    // A window that has gone still is sent on, a couple of pictures a second at the least.
    partBegins(QStringLiteral("still"));
    window.setAnimated(false);
    QTest::qWait(300);
    size_t before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileStill = record.frames.size() - before;
    QVERIFY2(whileStill >= 3 && whileStill <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileStill)));

    // So is one that is off the screen, and one that Windows has stopped drawing, of which
    // graphics capture has nothing new at all. A key frame asked for then comes of the last
    // picture, and does not wait for the next.
    partBegins(QStringLiteral("off the screen"));
    window.moveTo(QPoint(-6000, -6000));
    QTest::qWait(500);
    before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileAway = record.frames.size() - before;
    QVERIFY2(whileAway >= 3 && whileAway <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileAway)));
    partBegins(QStringLiteral("back on the screen"));
    window.moveTo(home);

    partBegins(QStringLiteral("cloaked"));
    window.setCloaked(true);
    QTest::qWait(700);
    before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileCloaked = record.frames.size() - before;
    qInfo() << "frames in 2 s:" << whileStill << "of a window that was still," << whileAway << "of one off the screen," << whileCloaked << "of one Windows had stopped drawing";
    QVERIFY2(whileCloaked >= 3 && whileCloaked <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileCloaked)));

    const size_t atRequest = record.frames.size();
    const qint64 askedAt = record.clock.elapsed();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(record.keyFrameFrom(atRequest) < record.frames.size(), 1500);
    const qint64 waited = record.arrivedAt[record.keyFrameFrom(atRequest)] - askedAt;
    qInfo() << "a key frame" << waited << "ms after it was asked for";
    QVERIFY(waited < 400);
    // Sent again, and not a second time before the next is due.
    QTest::qWait(200);
    QCOMPARE(record.keyFrameFrom(record.keyFrameFrom(atRequest) + 1), record.frames.size());
    partBegins(QStringLiteral("uncloaked"));
    window.setCloaked(false);
    QTest::qWait(500);
    QCOMPARE(record.starts.size(), 1);
    QVERIFY(record.stalls.isEmpty());

    // The picture follows the window's size, and starts over with a key frame when it does.
    partBegins(QStringLiteral("resized"));
    const size_t atResize = record.frames.size();
    // Of another shape where the picture is scaled down, which would bring both sizes to the same.
    window.resizeInside(maxHeight ? QSize(800, 400) : QSize(800, 600));
    const QSize secondSize = encodeSizeFor(window.insideOnScreen().size(), maxHeight);
    QVERIFY(secondSize != firstSize);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), secondSize);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atResize + 20, 3000);
    QVERIFY(record.keyFrameFrom(atResize) < record.frames.size());

    // Something laid over the middle of the window: the window goes on being shared, by
    // itself, at the same size and without a key frame.
    const QRect inside = window.insideOnScreen();
    QTest::qWait(300);
    partBegins(QStringLiteral("covered"));
    const size_t atCover = record.frames.size();
    size_t coveredUntil = 0;
    {
        const TestWindow cover(TestWindow::Look{ inside.size() / 3, inside.center() - QPoint(inside.width() / 6, inside.height() / 6), COVERING_COLOUR, false });
        QVERIFY(cover.isOpen());
        QTest::qWait(400);
        PathCounts earlier;
        PathCounts later;
        QVERIFY(countsOverASecond(record, &earlier, &later));
        if (duplicated) {
            qInfo() << "covered:" << later.cutOut - earlier.cutOut << "pictures cut out of the screen and" << later.drawn - earlier.drawn << "drawn by the window in a second";
            QVERIFY(later.drawn > earlier.drawn);
            QCOMPARE(later.cutOut, earlier.cutOut);
        }
        coveredUntil = record.frames.size();
        partBegins(QStringLiteral("uncovered"));
    }
    QVERIFY(coveredUntil >= atCover + 20);
    QVERIFY(record.keyFrameFrom(atCover) >= coveredUntil);
    QCOMPARE(record.starts.size(), 2);
    const Decoded covered = decodeAll(codec, record.framesBefore(coveredUntil));
    QCOMPARE(covered.errors, quint64(0));
    QCOMPARE(QSize(covered.width, covered.height), secondSize);
    QVERIFY2(looksLike(covered.middle, SHARED_COLOUR), "the middle of the picture is not the shared window's colour");
    QVERIFY2(!looksLike(covered.middle, COVERING_COLOUR), "what covered the window is in the picture");
    QVERIFY2(looksLike(covered.top, SHARED_COLOUR), "the top of the picture is not the inside of the window");
    qInfo() << "covered, the middle decodes off the window's colour by" << offItsOwnCoefficients(covered, covered.middle, SHARED_COLOUR) << "; the stream says its colours are" << covered.colorspace;
    QVERIFY2(offItsOwnCoefficients(covered, covered.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window's colour did not survive while it was covered");

    if (duplicated) {
        QTest::qWait(300);
        PathCounts earlier;
        PathCounts later;
        QVERIFY(countsOverASecond(record, &earlier, &later));
        qInfo() << "uncovered:" << later.cutOut - earlier.cutOut << "pictures cut out of the screen and" << later.drawn - earlier.drawn << "drawn by the window in a second";
        // Something of the desktop's own may pass over the window for a moment.
        QVERIFY(later.cutOut - earlier.cutOut >= FRAMERATE / 2);
        QVERIFY(later.drawn - earlier.drawn <= FRAMERATE / 6);
    }

    // A window that reaches a little past the edge of its screen is still cut out of it, with
    // black where the screen ends, and one that reaches further draws itself.
    if (duplicated && window.reachPastAnOuterEdge(quiet->whole, 10)) {
        PathCounts earlier;
        PathCounts later;
        QTest::qWait(400);
        QVERIFY(countsOverASecond(record, &earlier, &later));
        QVERIFY(later.cutOut - earlier.cutOut >= FRAMERATE / 2);
        QVERIFY(later.drawn - earlier.drawn <= FRAMERATE / 6);
        QVERIFY(window.reachPastAnOuterEdge(quiet->whole, 40));
        QTest::qWait(400);
        QVERIFY(countsOverASecond(record, &earlier, &later));
        QVERIFY(later.drawn > earlier.drawn);
        QCOMPARE(later.cutOut, earlier.cutOut);
        window.moveTo(home);
        QTest::qWait(400);
        QCOMPARE(record.starts.size(), 2);
    }

    // Hidden it is paused, and resumes when it shows again, for which a graphics capture has
    // to be made anew: Windows ends the one of a window that is hidden. Where it is duplicated
    // it is paused as well while it asks to be kept out of captures.
    QList<bool> stalls;
    QVERIFY(record.stalls.isEmpty());
    for (int pause = 0; pause < (duplicated ? 2 : 1); pause++) {
        partBegins(pause == 0 ? QStringLiteral("hidden") : QStringLiteral("kept out of captures"));
        if (pause == 0)
            window.hide();
        else if (!window.keepOutOfCaptures(true))
            break;
        stalls.append(true);
        QTRY_COMPARE_WITH_TIMEOUT(record.stalls, stalls, 2000);
        QTest::qWait(300);
        const size_t whilePaused = record.frames.size();
        QTest::qWait(700);
        QCOMPARE(record.frames.size(), whilePaused);

        partBegins(pause == 0 ? QStringLiteral("shown again") : QStringLiteral("let into captures again"));
        if (pause == 0)
            window.show();
        else
            window.keepOutOfCaptures(false);
        stalls.append(false);
        QTRY_COMPARE_WITH_TIMEOUT(record.stalls, stalls, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= whilePaused + 10, 3000);
        QCOMPARE(record.starts.size(), 2);
    }

    // Closed, the share is over, and nothing went wrong.
    partBegins(QStringLiteral("closed"));
    window.close();
    QTRY_COMPARE_WITH_TIMEOUT(record.closes, 1, 3000);
    QTest::qWait(300);
    const size_t afterClose = record.frames.size();
    QTest::qWait(300);
    QCOMPARE(record.frames.size(), afterClose);
    QCOMPARE(record.closes, 1);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.stalls, stalls);

    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));
    // Graphics capture has a picture with nothing in it for a window as it is hidden, as it is
    // shown again and as it is closed. Sent on, a viewer sees black where the window was.
    QMutex blackMutex;
    QStringList blackPictures;
    const Decoded decoded = decodeAll(codec, record.frames, [&blackMutex, &blackPictures, &parts](const AVFrame *picture, int index) {
        if (picture->data[0][(picture->height / 2) * picture->linesize[0] + picture->width / 2] >= BLACK_AT_MOST)
            return;
        QString part;
        for (const auto &[from, name] : parts) {
            if (from <= static_cast<size_t>(index))
                part = QStringLiteral("%1 after it was %2").arg(static_cast<size_t>(index) - from).arg(name);
        }
        const QMutexLocker locker(&blackMutex);
        blackPictures.append(QStringLiteral("picture %1 (%2)").arg(index).arg(part));
    });
    // A duplicated window is not held to this. Its screen shows it black from the moment it
    // asks to be kept out of captures, and for a picture as Windows starts drawing it again.
    if (!duplicated)
        QVERIFY2(blackPictures.isEmpty(), qPrintable(QStringLiteral("black: ") + blackPictures.join(QStringLiteral(", "))));
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(QSize(decoded.width, decoded.height), secondSize);
    QVERIFY2(looksLike(decoded.middle, SHARED_COLOUR), "the middle of the last picture is not the shared window's colour");
    QVERIFY2(looksLike(decoded.top, SHARED_COLOUR), "the top of the last picture is not the inside of the window");
    qInfo() << "uncovered, the middle decodes off the window's colour by" << offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY2(offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window's colour did not survive");
    // Whatever the window went through, its pictures stayed where they were to begin with.
    QCOMPARE(record.stats.last().encoderName.contains(QStringLiteral("(d3d11va)")), onCard);
#else
    QSKIP("windows are captured on Windows");
#endif
}

// A window whose size AV1's encoder would add black columns to: its size is reported, so that
// another codec can be asked for, and none of it is encoded until it has a size that is coded
// as it is.
void TestVideoEncode::thePipelineHoldsBackWhatItsEncoderWouldPad()
{
#ifdef Q_OS_WIN
    using namespace Core::Video;
    using Core::Media::codedSize;
    const QString encoderName = Core::Media::videoEncoderFor(VideoCodec::AV1);
    const QSize padded(700, 500);
    const QSize exact(640, 480);
    if (encoderName.isEmpty() || codedSize(encoderName, padded) == padded)
        QSKIP("this machine's AV1 encoder, if it has one, codes every size as it is");
    if (!windowCaptureAvailable())
        QSKIP("this build or this Windows cannot capture a window");
    QVERIFY(!Core::Media::paddingIsAcceptable(padded, codedSize(encoderName, padded)));
    QCOMPARE(codedSize(encoderName, exact), exact);

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    TestWindow window(TestWindow::Look{ padded, quiet->cornerFor(QSize(860, 700)) });
    QVERIFY(window.isOpen());
    QTest::qWait(200);
    QCOMPARE(window.insideOnScreen().size(), padded);

    CaptureSource source;
    source.id = window.sourceId();
    source.kind = CaptureSource::Kind::Window;
    QCOMPARE(encodeSizeOf(source, 0), padded);
    QCOMPARE(encodeSizeOf(source, 360), encodeSizeFor(padded, 360));

    BroadcastPipeline pipeline;
    PipelineRecord record(pipeline);
    BroadcastSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = 0;
    settings.framerate = FRAMERATE;
    settings.codec = VideoCodec::AV1;
    settings.bitrate = 2'500'000;
    settings.maxBitrate = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(!record.starts.isEmpty() || !record.failures.isEmpty(), "the size was not reported", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.first(), padded);
    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    QTest::qWait(500);
    QVERIFY(record.frames.empty());
    QVERIFY2(record.stats.last().encoderName.startsWith(QStringLiteral("none while AV1")), qPrintable(record.stats.last().encoderName));
    QVERIFY(record.stats.last().capturedFps > 0);
    QCOMPARE(record.stats.last().framesEncoded, quint64(0));

    window.resizeInside(exact);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), exact);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= 10, 3000);
    QVERIFY(record.frames[0]->keyframe);
    pipeline.stop();
    QTest::qWait(200);

    const Decoded decoded = decodeAll(VideoCodec::AV1, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(QSize(decoded.width, decoded.height), exact);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
#else
    QSKIP("windows are captured on Windows");
#endif
}

void TestVideoEncode::whatCapturingAWindowCosts_data()
{
    using Core::Video::CaptureMethod;
    QTest::addColumn<CaptureMethod>("method");
    QTest::addColumn<bool>("covered");
    QTest::addColumn<int>("maxHeight");
    QTest::newRow("graphics capture, at the window's own size") << CaptureMethod::GraphicsCapture << false << 0;
    QTest::newRow("graphics capture, scaled down") << CaptureMethod::GraphicsCapture << false << 720;
    QTest::newRow("cut out of the screen, at the window's own size") << CaptureMethod::DesktopDuplication << false << 0;
    QTest::newRow("cut out of the screen, scaled down") << CaptureMethod::DesktopDuplication << false << 720;
    QTest::newRow("drawn by the window, at its own size") << CaptureMethod::DesktopDuplication << true << 0;
    QTest::newRow("drawn by the window, scaled down") << CaptureMethod::DesktopDuplication << true << 720;
}

// Not a check of much: a record of what each way of capturing a window costs this process.
// Drawing a window is mostly the work of Windows' compositor, in another process, and shows
// here only as a frame rate that is not held.
void TestVideoEncode::whatCapturingAWindowCosts()
{
#ifdef Q_OS_WIN
    QFETCH(Core::Video::CaptureMethod, method);
    QFETCH(bool, covered);
    QFETCH(int, maxHeight);
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (!windowCaptureAvailable() || !captureMethodUsable(method))
        QSKIP("this build or this Windows cannot capture a window this way");

    // As large as a window that is shared tends to be, where the second screen has the room,
    // and scaled to 720p where that is smaller than the window.
    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const QSize room(std::min(1700, quiet->workArea.width() - 48), std::min(1000, quiet->workArea.height() - 48));
    const TestWindow window(TestWindow::Look{ QSize((room.width() - 100) & ~1, (room.height() - 100) & ~1), quiet->cornerFor(room) });
    QVERIFY(window.isOpen());
    QTest::qWait(200);
    const QRect inside = window.insideOnScreen();
    if (maxHeight >= inside.height())
        maxHeight = inside.height() / 2;
    std::unique_ptr<TestWindow> cover;
    if (covered)
        cover = std::make_unique<TestWindow>(TestWindow::Look{ QSize(64, 64), inside.center(), COVERING_COLOUR, false });

    setCaptureMethod(method);
    BroadcastPipeline pipeline;
    PipelineRecord record(pipeline);
    BroadcastSettings settings;
    settings.sourceId = window.sourceId();
    settings.maxHeight = maxHeight;
    settings.framerate = FRAMERATE;
    settings.codec = codecsSharedAt(encodeSizeFor(inside.size(), maxHeight)).first();
    settings.bitrate = 4'000'000;
    settings.maxBitrate = 6'000'000;
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty(), "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.first(), encodeSizeFor(inside.size(), maxHeight));

    const double cpuBefore = processCpuMs();
    const size_t framesBefore = record.frames.size();
    QElapsedTimer timer;
    timer.start();
    QTest::qWait(2500);
    const size_t frames = std::max<size_t>(1, record.frames.size() - framesBefore);
    const double cpuPerFrame = (processCpuMs() - cpuBefore) / static_cast<double>(frames);
    const double framesPerSecond = static_cast<double>(frames) * 1000.0 / static_cast<double>(timer.elapsed());
    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    const BroadcastStats last = record.stats.last();
    qInfo().noquote() << QStringLiteral("a %1x%2 window sent as %3x%4: %5 ms of processor time a picture in all, %6 fps, %7 ms in the encoder a picture; %8").arg(inside.width()).arg(inside.height()).arg(last.encodedSize.width()).arg(last.encodedSize.height()).arg(cpuPerFrame, 0, 'f', 2).arg(framesPerSecond, 0, 'f', 1).arg(last.averageEncodeMs, 0, 'f', 2).arg(last.encoderName);

    if (method == CaptureMethod::DesktopDuplication) {
        const PathCounts counts = pathCounts(last);
        QVERIFY(covered ? counts.drawn > 0 && counts.cutOut == 0 : counts.cutOut > 10 * counts.drawn);
    }
    QCOMPARE(last.encoderName.contains(QStringLiteral("(d3d11va)")), reachesTheEncoderOnTheCard(settings.codec, last.encodedSize != inside.size(), method == CaptureMethod::GraphicsCapture));
    QVERIFY(framesPerSecond > FRAMERATE * 0.5 && framesPerSecond < FRAMERATE * 1.25);
#else
    QSKIP("windows are captured on Windows");
#endif
}

QTEST_GUILESS_MAIN(TestVideoEncode)
#include "tst_VideoEncode.moc"
