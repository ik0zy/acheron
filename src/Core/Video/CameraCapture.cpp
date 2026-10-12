#include "Core/Video/CameraCapture.hpp"

#include "Core/Logging.hpp"
#include "Core/Video/CameraPipeline.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#if defined(ACHERON_HAVE_AVDEVICE) && defined(Q_OS_WIN)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace Acheron {
namespace Core {
namespace Video {

namespace {

std::mutex factoryMutex;
CameraFrameSourceFactory replacementFactory;

#if defined(ACHERON_HAVE_AVDEVICE) && defined(Q_OS_WIN)

using Clock = std::chrono::steady_clock;

struct Mode
{
    // Null asks for nothing, and the camera gives what it gives by default.
    const char *size;
    const char *framerate;
    // AV_CODEC_ID_NONE takes the first format the camera has at that size and rate.
    AVCodecID codec;
};

constexpr std::array MODES = {
    Mode{ "1280x720", "30", AV_CODEC_ID_MJPEG },
    Mode{ "1280x720", "30", AV_CODEC_ID_NONE },
    Mode{ "640x480", "30", AV_CODEC_ID_NONE },
    Mode{ nullptr, nullptr, AV_CODEC_ID_NONE },
};

// When ffmpeg's buffer for the device is full it is the newest picture that is dropped and the
// older ones that stay, so the buffer is kept to two uncompressed 1280x720 pictures of two
// bytes a pixel, and is read as soon as anything is in it.
constexpr int DEVICE_BUFFER_BYTES = 2 * 1280 * 720 * 2;

// ffmpeg's blocking read of a DirectShow device cannot be woken, and a camera that goes quiet
// would hold its reader, and with it whoever stops the camera, for good. So the device is asked
// without waiting, this often.
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1);

struct InputCloser
{
    void operator()(AVFormatContext *input) const { avformat_close_input(&input); }
};
struct DecoderDeleter
{
    void operator()(AVCodecContext *decoder) const { avcodec_free_context(&decoder); }
};
struct PacketDeleter
{
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};

struct OpenCamera
{
    std::unique_ptr<AVFormatContext, InputCloser> input;
    std::unique_ptr<AVCodecContext, DecoderDeleter> decoder;
};

const AVInputFormat *inputDevice()
{
    static const AVInputFormat *const device = [] {
        avdevice_register_all();
        return av_find_input_format("dshow");
    }();
    return device;
}

AVFormatContext *openInput(const QByteArray &url, const Mode &mode)
{
    AVFormatContext *input = avformat_alloc_context();
    if (!input)
        return nullptr;
    // The device has no option for a compressed format: it takes the one named here.
    input->video_codec_id = mode.codec;
    input->flags |= AVFMT_FLAG_NONBLOCK;

    AVDictionary *options = nullptr;
    if (mode.size) {
        av_dict_set(&options, "video_size", mode.size, 0);
        av_dict_set(&options, "framerate", mode.framerate, 0);
    }
    av_dict_set_int(&options, "rtbufsize", DEVICE_BUFFER_BYTES, 0);
    // A context that could not be opened is freed by the call.
    avformat_open_input(&input, url.constData(), inputDevice(), &options);
    av_dict_free(&options);
    return input;
}

AVCodecContext *openDecoder(const AVStream *stream)
{
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder)
        return nullptr;
    // A decoder that shares its work out among threads hands each picture back that many
    // pictures late.
    decoder->thread_count = 1;
    decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_parameters_to_context(decoder, stream->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0)
        avcodec_free_context(&decoder);
    return decoder;
}

// The camera in the first mode it opens in and whose pictures can be decoded, or nothing.
OpenCamera openCamera(const QByteArray &url)
{
    for (const Mode &mode : MODES) {
        OpenCamera camera;
        camera.input.reset(openInput(url, mode));
        if (!camera.input || camera.input->nb_streams == 0 || camera.input->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        camera.decoder.reset(openDecoder(camera.input->streams[0]));
        if (camera.decoder)
            return camera;
    }
    return {};
}

QString describe(const AVStream *stream)
{
    const AVCodecParameters *sent = stream->codecpar;
    const char *format = sent->codec_id == AV_CODEC_ID_RAWVIDEO ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(sent->format)) : avcodec_get_name(sent->codec_id);
    return QStringLiteral("dshow %1 %2x%3 at %4 fps").arg(QString::fromLatin1(format ? format : "unknown")).arg(sent->width).arg(sent->height).arg(av_q2d(stream->avg_frame_rate), 0, 'g', 4);
}

#endif

} // namespace

std::chrono::steady_clock::time_point CaptureClock::at(std::optional<std::chrono::microseconds> stamped, std::chrono::steady_clock::time_point readAt)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point taken = readAt;
    if (stamped) {
        const Clock::duration ahead = readAt.time_since_epoch() - *stamped;
        const Clock::duration followed = ourClockAhead ? *ourClockAhead + (readAt - lastReadAt) / SLOWEST_CLOCK_FOLLOWED : ahead;
        ourClockAhead = ahead - followed > STARTED_OVER ? ahead : std::min(ahead, followed);
        taken = Clock::time_point(std::chrono::duration_cast<Clock::duration>(*stamped + *ourClockAhead));
    }
    lastReadAt = readAt;
    last = last ? std::max(*last + std::chrono::microseconds(1), taken) : taken;
    return *last;
}

void setCameraFrameSourceFactory(CameraFrameSourceFactory factory)
{
    std::lock_guard lock(factoryMutex);
    replacementFactory = std::move(factory);
}

std::unique_ptr<CameraFrameSource> makeCameraFrameSource()
{
    {
        std::lock_guard lock(factoryMutex);
        if (replacementFactory)
            return replacementFactory();
    }
#if defined(ACHERON_HAVE_AVDEVICE) && defined(Q_OS_WIN)
    return std::make_unique<CameraCapture>();
#else
    return nullptr;
#endif
}

#if defined(ACHERON_HAVE_AVDEVICE) && defined(Q_OS_WIN)

bool cameraCaptureAvailable()
{
    return inputDevice() != nullptr;
}

QList<CameraDevice> cameraDevices()
{
    AVDeviceInfoList *found = nullptr;
    if (!inputDevice() || avdevice_list_input_sources(inputDevice(), nullptr, nullptr, &found) < 0)
        return {};

    // Microphones are in the same list.
    QList<CameraDevice> cameras;
    for (int i = 0; i < found->nb_devices; i++) {
        const AVDeviceInfo *device = found->devices[i];
        const AVMediaType *const types = device->media_types;
        if (std::find(types, types + device->nb_media_types, AVMEDIA_TYPE_VIDEO) != types + device->nb_media_types)
            cameras.append({ QString::fromUtf8(device->device_name), QString::fromUtf8(device->device_description) });
    }
    avdevice_free_list_devices(&found);
    return cameras;
}

CameraCapture::CameraCapture() : newest(av_frame_alloc()) {}

CameraCapture::~CameraCapture()
{
    stopping = true;
    if (thread.joinable())
        thread.join();
    av_frame_free(&newest);
}

bool CameraCapture::open(const QString &deviceId)
{
    // ffmpeg enters a COM apartment where the device is opened and leaves it where the device
    // is closed, so one thread does both, and the reading in between.
    thread = std::thread([this, deviceId] { run(deviceId); });

    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return state != State::Opening; });
    return state == State::Open;
}

CameraFrameSource::Status CameraCapture::read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt)
{
    std::unique_lock lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(READ_WAIT_MS), [this] { return newest->buf[0] || state == State::Ended; });
    if (newest->buf[0]) {
        av_frame_move_ref(into, newest);
        capturedAt = newestCapturedAt;
        return Status::Frame;
    }
    return state == State::Ended ? Status::Ended : Status::NoFrame;
}

QString CameraCapture::failure() const
{
    std::lock_guard lock(mutex);
    return failureText;
}

QString CameraCapture::description() const
{
    std::lock_guard lock(mutex);
    return mode;
}

void CameraCapture::opened(const QString &openedMode)
{
    {
        std::lock_guard lock(mutex);
        mode = openedMode;
        state = State::Open;
    }
    changed.notify_all();
}

void CameraCapture::end(const QString &reason)
{
    {
        std::lock_guard lock(mutex);
        failureText = reason;
        state = State::Ended;
    }
    changed.notify_all();
}

void CameraCapture::publish(AVFrame *picture, std::chrono::steady_clock::time_point capturedAt)
{
    // The matrix the USB video class gives a camera that names none, at any size.
    if (picture->colorspace == AVCOL_SPC_UNSPECIFIED)
        picture->colorspace = AVCOL_SPC_SMPTE170M;
    {
        std::lock_guard lock(mutex);
        av_frame_unref(newest);
        av_frame_move_ref(newest, picture);
        newestCapturedAt = capturedAt;
    }
    changed.notify_all();
}

void CameraCapture::run(const QString &deviceId)
{
    const QList<CameraDevice> cameras = cameraDevices();
    if (deviceId.isEmpty() && cameras.isEmpty()) {
        end(QCoreApplication::translate("Acheron::Core::Video::CameraPipeline", "No camera is connected."));
        return;
    }
    const QString id = deviceId.isEmpty() ? cameras.first().id : deviceId;
    const auto listed = std::find_if(cameras.begin(), cameras.end(), [&id](const CameraDevice &each) { return each.id == id; });

    const OpenCamera camera = openCamera("video=" + id.toUtf8());
    if (!camera.input) {
        end(listed == cameras.end() ? QCoreApplication::translate("Acheron::Core::Video::CameraPipeline", "The camera is not connected.") : QCoreApplication::translate("Acheron::Core::Video::CameraPipeline", "The camera could not be opened. Another program may be using it."));
        return;
    }
    const AVStream *stream = camera.input->streams[0];
    const QString openedMode = describe(stream);
    qCInfo(LogVideo).noquote() << "Camera" << (listed == cameras.end() ? id : listed->name) << "opened as" << openedMode;
    opened(openedMode);

    const std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    const std::unique_ptr<AVPacket, PacketDeleter> newer(av_packet_alloc());
    const std::unique_ptr<AVFrame, FrameDeleter> decoded(av_frame_alloc());
    const AVCodecDescriptor *codec = avcodec_descriptor_get(camera.decoder->codec_id);
    const bool everyPictureStandsAlone = codec && (codec->props & AV_CODEC_PROP_INTRA_ONLY);
    CaptureClock clock;
    QElapsedTimer sincePicture;
    sincePicture.start();

    while (!stopping) {
        const int got = av_read_frame(camera.input.get(), packet.get());
        if (got == AVERROR(EAGAIN)) {
            if (sincePicture.elapsed() >= NO_PICTURE_TIMEOUT_MS) {
                end(QCoreApplication::translate("Acheron::Core::Video::CameraPipeline", "The camera is not sending pictures. Another program may be using it."));
                return;
            }
            std::this_thread::sleep_for(POLL_INTERVAL);
            continue;
        }
        if (got < 0) {
            end(QCoreApplication::translate("Acheron::Core::Video::CameraPipeline", "The camera was disconnected."));
            return;
        }
        sincePicture.start();
        const Clock::time_point readAt = Clock::now();

        // A picture with a newer one already waiting behind it is late, and is passed over
        // where the newer one can be decoded without it.
        while (everyPictureStandsAlone && av_read_frame(camera.input.get(), newer.get()) >= 0) {
            av_packet_unref(packet.get());
            av_packet_move_ref(packet.get(), newer.get());
        }

        const int sent = avcodec_send_packet(camera.decoder.get(), packet.get());
        av_packet_unref(packet.get());
        while (sent >= 0 && avcodec_receive_frame(camera.decoder.get(), decoded.get()) >= 0) {
            std::optional<std::chrono::microseconds> stamped;
            if (decoded->pts != AV_NOPTS_VALUE)
                stamped = std::chrono::microseconds(av_rescale_q(decoded->pts, stream->time_base, AVRational{ 1, 1'000'000 }));
            publish(decoded.get(), clock.at(stamped, readAt));
        }
    }
}

#else

bool cameraCaptureAvailable()
{
    return false;
}

QList<CameraDevice> cameraDevices()
{
    return {};
}

#endif

} // namespace Video
} // namespace Core
} // namespace Acheron
