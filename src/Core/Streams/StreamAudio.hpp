#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <memory>
#include <numeric>
#include <utility>

namespace Acheron {
namespace Discord {
namespace Voice {
class VoiceClient;
}
} // namespace Discord

namespace Core {

namespace Audio {
class OpusEncoder;
}

// Whether a frame of 16-bit samples has nothing in it to hear. Frames are captured for as long
// as anything at all is played on the machine, and silence does not come as zeros: it is the
// noise that rounding to 16 bits leaves, which was measured at a root mean square of 0.5 for a
// program that plays nothing and of up to 1.8 for everything but one program.
[[nodiscard]] inline bool isSilence(const QByteArray &pcm)
{
    constexpr qint64 LOUDEST_SILENCE_RMS = 6;

    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const qint64 count = pcm.size() / static_cast<qint64>(sizeof(qint16));
    const qint64 squares = std::accumulate(samples, samples + count, qint64(0), [](qint64 sum, qint16 sample) { return sum + qint64(sample) * sample; });
    return squares <= LOUDEST_SILENCE_RMS * LOUDEST_SILENCE_RMS * count;
}

// Cuts what is captured into runs of sound, as the microphone's frames are cut into talk
// spurts: a run starts with the first frame that has something in it and goes on through a
// short silence, so that a pause in the music does not end it.
class SoundRuns
{
public:
    enum class Step {
        // Silence outside a run. Nothing is sent for it.
        Skip,
        // The first frame of a run.
        Begin,
        Continue,
        // The silence has lasted too long: the run is over, and this frame is not part of it.
        End,
    };

    // As long as a talk spurt is held open after the last of the voice.
    static constexpr int HELD_SILENT_FRAMES = 25;

    Step offer(bool silent)
    {
        if (!silent) {
            silentFramesLeft = HELD_SILENT_FRAMES;
            return std::exchange(open, true) ? Step::Continue : Step::Begin;
        }
        if (!open)
            return Step::Skip;
        if (silentFramesLeft > 0) {
            silentFramesLeft--;
            return Step::Continue;
        }
        open = false;
        return Step::End;
    }

    // Frames have stopped coming, or cannot be sent. True when that ends a run.
    bool stop() { return std::exchange(open, false); }

private:
    bool open = false;
    int silentFramesLeft = 0;
};

// The sound that goes with the account's own stream. Takes what is captured, as 20 ms frames
// of 48 kHz stereo, and sends the runs of sound in it as Opus on the stream's RTC connection.
// It works on that connection's thread, which adopting it moves it to.
class StreamAudio : public QObject
{
    Q_OBJECT
public:
    // What the official client raises a stream's audio to when sound goes with it, unless the
    // voice bitrate is higher still.
    static constexpr int MIN_BITRATE = 128000;
    // Nothing is captured while the machine plays nothing at all. This long without a frame
    // ends a run.
    static constexpr int SOUND_GONE_MS = 100;

    StreamAudio(Discord::Voice::VoiceClient *rtc, int bitrate);
    ~StreamAudio() override;

public slots:
    void onFrameCaptured(const QByteArray &pcm);

private slots:
    void onSoundGone();

private:
    void sendTrailingSilence();

    Discord::Voice::VoiceClient *rtc;
    std::unique_ptr<Audio::OpusEncoder> encoder;
    SoundRuns runs;
    QTimer soundGone;

    static constexpr int TRAILING_SILENCE_FRAMES = 5;
};

} // namespace Core
} // namespace Acheron
