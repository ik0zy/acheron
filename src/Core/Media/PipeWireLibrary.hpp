#pragma once

#include <dlfcn.h>

namespace Acheron {
namespace Core {
namespace Media {

// Whether libpipewire can be called without ending the process. It has to be the machine's own,
// so a build made to be carried to other machines (IMPLIB_GEN in linuxcapture.cmake) loads it
// only when it is first called, and aborts on a machine that has none.
[[nodiscard]] inline bool pipeWireCanBeCalled()
{
    static const bool loadable = dlopen("libpipewire-0.3.so.0", RTLD_LAZY) != nullptr;
    return loadable;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
