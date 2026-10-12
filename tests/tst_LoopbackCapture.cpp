// The capture against this machine's own sound: the test plays a short, quiet tone and looks
// for it in what is delivered. Then the rules that cut captured sound into runs, which need
// nothing of the machine.

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/LoopbackCapture.hpp"
#include "Core/Streams/StreamAudio.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>

#ifdef Q_OS_WIN
#define MINIAUDIO_IMPLEMENTATION
#include "Core/Audio/Miniaudio.hpp"

#include <tlhelp32.h>
#endif

using namespace Acheron::Core;
using Audio::LoopbackCapture;

namespace {

constexpr double TONE_HZ = 1375;
constexpr float TONE_AMPLITUDE = 0.02f;
constexpr int TONE_MS = 400;
constexpr int TONE_FADE_MS = 15;
// For Windows to have made the capture before there is anything to hear.
constexpr int CAPTURE_STARTS_MS = 250;
constexpr int SOUND_ARRIVES_MS = 200;

const char CANNOT_CAPTURE[] = "This machine cannot capture one program's sound.";
const char CANNOT_PLAY[] = "This machine has no sound output to play a tone on.";

#ifdef Q_OS_WIN

// Plays the tone once on the default output, from this process, then silence.
class Tone
{
public:
    Tone()
    {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = Audio::AUDIO_CHANNELS;
        config.sampleRate = Audio::AUDIO_SAMPLE_RATE;
        config.dataCallback = &Tone::fill;
        config.pUserData = this;
        opened = ma_device_init(nullptr, &config, &device) == MA_SUCCESS;
        playing = opened && ma_device_start(&device) == MA_SUCCESS;
    }

    ~Tone()
    {
        if (opened)
            ma_device_uninit(&device);
    }

    [[nodiscard]] bool isPlaying() const { return playing; }

private:
    static void fill(ma_device *device, void *output, const void *, ma_uint32 sampleFrames)
    {
        constexpr int LENGTH = TONE_MS * Audio::AUDIO_SAMPLE_RATE / 1000;
        constexpr float FADE = TONE_FADE_MS * Audio::AUDIO_SAMPLE_RATE / 1000;

        auto *tone = static_cast<Tone *>(device->pUserData);
        auto *samples = static_cast<float *>(output);
        for (ma_uint32 i = 0; i < sampleFrames; i++) {
            float sample = 0;
            if (tone->position < LENGTH) {
                // Faded in and out, to spare whoever is in the room the clicks.
                const float envelope = qMin(1.0f, qMin(tone->position, LENGTH - tone->position) / FADE);
                sample = TONE_AMPLITUDE * envelope * static_cast<float>(std::sin(2 * std::numbers::pi * TONE_HZ * tone->position / Audio::AUDIO_SAMPLE_RATE));
                tone->position++;
            }
            samples[2 * i] = sample;
            samples[2 * i + 1] = sample;
        }
    }

    ma_device device;
    bool opened = false;
    bool playing = false;
    int position = 0;
};

int threadsOfThisProcess()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    int count = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId())
            count++;
    }
    CloseHandle(snapshot);
    return count;
}

bool processExists(quint32 processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process)
        CloseHandle(process);
    return process != nullptr;
}

#else

class Tone
{
public:
    [[nodiscard]] bool isPlaying() const { return false; }
};

int threadsOfThisProcess()
{
    return 0;
}

bool processExists(quint32)
{
    return false;
}

#endif

// What one capture delivers, gathered on the test's thread.
struct Recording : QObject
{
    Recording()
    {
        connect(&capture, &LoopbackCapture::frameCaptured, this, [this](const QByteArray &pcm) { frames.append(pcm); });
        connect(&capture, &LoopbackCapture::failed, this, [this](const QString &reason) { failures.append(reason); });
    }

    [[nodiscard]] bool everyFrameIsWhole() const
    {
        return std::all_of(frames.cbegin(), frames.cend(), [](const QByteArray &pcm) { return pcm.size() == Audio::AUDIO_FRAME_SIZE; });
    }

    // The frames with something in them, end to end.
    [[nodiscard]] QByteArray sound() const
    {
        QByteArray joined;
        for (const QByteArray &pcm : frames) {
            if (!isSilence(pcm))
                joined.append(pcm);
        }
        return joined;
    }

    LoopbackCapture capture;
    QList<QByteArray> frames;
    QStringList failures;
};

// How much of one frequency there is in 16-bit stereo sound, as the amplitude of a steady tone.
double level(const QByteArray &pcm, double hz)
{
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const int sampleFrames = static_cast<int>(pcm.size()) / (Audio::AUDIO_CHANNELS * Audio::AUDIO_SAMPLE_BYTES);
    if (sampleFrames == 0)
        return 0;

    double real = 0;
    double imaginary = 0;
    for (int i = 0; i < sampleFrames; i++) {
        const double mono = (samples[2 * i] + samples[2 * i + 1]) / 2.0;
        const double angle = 2 * std::numbers::pi * hz * i / Audio::AUDIO_SAMPLE_RATE;
        real += mono * std::cos(angle);
        imaginary += mono * std::sin(angle);
    }
    return 2 * std::hypot(real, imaginary) / sampleFrames;
}

// Other programs may be playing anything, so the tone is told by standing out of what is
// beside it as well as by its size.
bool hasTone(const QByteArray &pcm)
{
    constexpr double BESIDE_HZ = 97;
    const double tone = level(pcm, TONE_HZ);
    const double beside = qMax(level(pcm, TONE_HZ - BESIDE_HZ), level(pcm, TONE_HZ + BESIDE_HZ));
    return tone > 0.3 * TONE_AMPLITUDE * 32767 && tone > 4 * beside;
}

QByteArray frameOf(qint16 sample)
{
    QByteArray pcm(Audio::AUDIO_FRAME_SIZE, '\0');
    std::fill_n(reinterpret_cast<qint16 *>(pcm.data()), pcm.size() / 2, sample);
    return pcm;
}

} // namespace

class TestLoopbackCapture : public QObject
{
    Q_OBJECT
private slots:
    void whetherItIsAvailableIsFoundOutOnce();
    void whatThisProgramPlaysIsCaptured();
    void everythingButThisProgramLeavesItsToneOut();
    void aProcessThatDoesNotExistHasNoSound();
    void startingAndStoppingLeavesNoThreadBehind();

    void silenceIsWhatRoundingLeaves();
    void aRunStartsWithSoundAndOutlastsAShortSilence();
    void soundDuringTheSilenceKeepsTheRunOpen();
    void aRunEndsWhenFramesStopComing();
};

void TestLoopbackCapture::whetherItIsAvailableIsFoundOutOnce()
{
    const bool available = Audio::processLoopbackAvailable();
#ifndef Q_OS_WIN
    QVERIFY(!available);
#endif

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 1000; i++)
        QCOMPARE(Audio::processLoopbackAvailable(), available);
    QVERIFY(timer.elapsed() < 100);
}

void TestLoopbackCapture::whatThisProgramPlaysIsCaptured()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    // Stopped and started again before there is anything to hear, so what is heard is the
    // second start's.
    Recording own;
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);
    own.capture.stop();
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(TONE_MS + SOUND_ARRIVES_MS);
    own.capture.stop();

    QVERIFY2(own.failures.isEmpty(), qPrintable(own.failures.join(' ')));
    QVERIFY(own.everyFrameIsWhole());
    // All of the tone but its fading ends, give or take a frame.
    const int framesOfTone = TONE_MS / Audio::AUDIO_FRAME_DURATION_MS;
    const int framesOfSound = static_cast<int>(own.sound().size()) / Audio::AUDIO_FRAME_SIZE;
    QVERIFY2(framesOfSound >= framesOfTone - 2 && framesOfSound <= framesOfTone + 2, qPrintable(QString::number(framesOfSound)));
    QVERIFY(hasTone(own.sound()));
}

void TestLoopbackCapture::everythingButThisProgramLeavesItsToneOut()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    // Both at once, so that the tone the one must not have is known to have been there.
    Recording own;
    Recording others;
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    others.capture.start(self, LoopbackCapture::Scope::AllButProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(TONE_MS + SOUND_ARRIVES_MS);
    own.capture.stop();
    others.capture.stop();

    QVERIFY2(own.failures.isEmpty(), qPrintable(own.failures.join(' ')));
    QVERIFY2(others.failures.isEmpty(), qPrintable(others.failures.join(' ')));
    QVERIFY(hasTone(own.sound()));
    QVERIFY(others.everyFrameIsWhole());
    QVERIFY(!hasTone(others.sound()));
}

void TestLoopbackCapture::aProcessThatDoesNotExistHasNoSound()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    constexpr quint32 NOBODY = 0x7FFFFFF0;
    QVERIFY(!processExists(NOBODY));

    Recording nobody;
    nobody.capture.start(NOBODY, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(TONE_MS + SOUND_ARRIVES_MS);
    nobody.capture.stop();

    // Windows makes the capture without complaint, and what it delivers while something else
    // is playing is silence.
    QVERIFY2(nobody.failures.isEmpty(), qPrintable(nobody.failures.join(' ')));
    QVERIFY(nobody.everyFrameIsWhole());
    QVERIFY(nobody.sound().isEmpty());
}

void TestLoopbackCapture::startingAndStoppingLeavesNoThreadBehind()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    // Stopped at every stage of starting, and destroyed in the middle of the last start.
    const auto startAndStop = [self] {
        LoopbackCapture capture;
        for (int wait = 0; wait <= 30; wait += 10) {
            capture.start(self, LoopbackCapture::Scope::AllButProcessTree);
            QTest::qWait(wait);
            capture.stop();
        }
        capture.start(self, LoopbackCapture::Scope::ProcessTree);
    };

    // Windows keeps a few threads of its own once sound has been touched.
    startAndStop();
    const int before = threadsOfThisProcess();

    constexpr int ROUNDS = 20;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < ROUNDS; i++)
        startAndStop();
    QVERIFY(timer.elapsed() < 10000);
    QVERIFY2(threadsOfThisProcess() - before < ROUNDS / 2, qPrintable(QStringLiteral("%1 threads before, %2 after").arg(before).arg(threadsOfThisProcess())));
}

void TestLoopbackCapture::silenceIsWhatRoundingLeaves()
{
    QVERIFY(isSilence(QByteArray()));
    QVERIFY(isSilence(frameOf(0)));
    QVERIFY(isSilence(frameOf(2)));
    QVERIFY(isSilence(frameOf(-6)));
    QVERIFY(!isSilence(frameOf(7)));
    QVERIFY(!isSilence(frameOf(-7)));

    // One click in a frame of nothing is something to hear, and so is the full scale.
    QByteArray click = frameOf(0);
    reinterpret_cast<qint16 *>(click.data())[Audio::AUDIO_FRAME_SIZE / 2 - 1] = 1000;
    QVERIFY(!isSilence(click));
    QVERIFY(!isSilence(frameOf(-32768)));
}

void TestLoopbackCapture::aRunStartsWithSoundAndOutlastsAShortSilence()
{
    SoundRuns runs;
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);

    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
    QCOMPARE(runs.offer(false), SoundRuns::Step::Continue);
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        QCOMPARE(runs.offer(true), SoundRuns::Step::Continue);
    QCOMPARE(runs.offer(true), SoundRuns::Step::End);
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);

    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
}

void TestLoopbackCapture::soundDuringTheSilenceKeepsTheRunOpen()
{
    SoundRuns runs;
    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        QCOMPARE(runs.offer(true), SoundRuns::Step::Continue);
    QCOMPARE(runs.offer(false), SoundRuns::Step::Continue);

    // The silence is counted from the last of the sound.
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        QCOMPARE(runs.offer(true), SoundRuns::Step::Continue);
    QCOMPARE(runs.offer(true), SoundRuns::Step::End);
}

void TestLoopbackCapture::aRunEndsWhenFramesStopComing()
{
    SoundRuns runs;
    QVERIFY(!runs.stop());

    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
    QVERIFY(runs.stop());
    QVERIFY(!runs.stop());
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);
    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
}

QTEST_GUILESS_MAIN(TestLoopbackCapture)
#include "tst_LoopbackCapture.moc"
