#pragma once

#ifdef ACHERON_HAVE_FFMPEG

#include <QString>

extern "C" {
#include <libavformat/avformat.h>
}

namespace Acheron {
namespace Core {
namespace Media {

// Keeps ffmpeg to the one resource it was asked to play.
//
// What a file is gets decided from its bytes, and several formats are lists of other things to
// fetch: an attachment named like a video can be an HLS or DASH playlist, an SDP description
// or a QuickTime file with outside references. Opening one would have ffmpeg connect to
// whatever addresses it names, from this machine and past any proxy. With this applied the
// input itself opens and nothing after it does, and only the protocols a plain download needs
// are available.
class InputConfinement
{
public:
    // Call between avformat_alloc_context() and avformat_open_input(), which must be given
    // `options`. The context must not outlive this object.
    void apply(AVFormatContext *format, AVDictionary **options, const QString &input);

private:
    static int openOnlyTheInput(AVFormatContext *format, AVIOContext **io, const char *url, int flags, AVDictionary **options);

    decltype(AVFormatContext::io_open) open = nullptr;
    bool inputOpened = false;
};

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
