#include "AudioPage.hpp"

#include "Core/Audio/AudioBackends.hpp"
#ifdef ACHERON_HAVE_STREAM_VIEW
#include "Core/Media/VideoDecoder.hpp"
#include "UI/Video/VideoSurface.hpp"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

AudioPage::AudioPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Backend *"), this));
    row->addStretch(1);

    backendCombo = new QComboBox(this);
    backendCombo->addItem(tr("Automatic"), QString());
    for (const QString &name : Core::Audio::supportedAudioBackends())
        backendCombo->addItem(name, name);

    const int current = backendCombo->findData(Core::Audio::configuredAudioBackend());
    backendCombo->setCurrentIndex(current >= 0 ? current : 0);

    row->addWidget(backendCombo);
    layout->addLayout(row);

    auto *note = new QLabel(tr("* Takes effect after a restart."), this);
    QFont noteFont = note->font();
    noteFont.setItalic(true);
    if (noteFont.pointSize() > 1)
        noteFont.setPointSize(noteFont.pointSize() - 1);
    note->setFont(noteFont);
    layout->addWidget(note);

#ifdef ACHERON_HAVE_STREAM_VIEW
    // Both fall back by themselves when the machine cannot do it; these are for turning them
    // off when it can but should not.
    auto *hardwareDecode = new QCheckBox(tr("Decode video with the graphics card"), this);
    hardwareDecode->setChecked(Core::Media::hardwareDecodingEnabled());
    layout->addWidget(hardwareDecode);
    connect(hardwareDecode, &QCheckBox::toggled, this, [](bool enabled) { Core::Media::setHardwareDecodingEnabled(enabled); });

    auto *gpuDrawing = new QCheckBox(tr("Draw video with the graphics card"), this);
    gpuDrawing->setChecked(VideoSurface::gpuDrawingEnabled());
    layout->addWidget(gpuDrawing);
    connect(gpuDrawing, &QCheckBox::toggled, this, [](bool enabled) { VideoSurface::setGpuDrawingEnabled(enabled); });

    auto *videoNote = new QLabel(tr("Video settings apply to streams and cameras opened afterwards."), this);
    videoNote->setFont(noteFont);
    layout->addWidget(videoNote);
#endif

    layout->addStretch(1);

    connect(backendCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        Core::Audio::setConfiguredAudioBackend(backendCombo->itemData(index).toString());
    });
}

} // namespace UI
} // namespace Acheron
