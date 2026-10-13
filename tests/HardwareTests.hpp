#pragma once

#include <QTest>

// The tests that need this machine's own graphics card, screens, camera or sound output are
// left out of an ordinary run: they take minutes, open windows and play a tone, and a build
// machine has none of what they need. ACHERON_HARDWARE_TESTS=1 in the environment runs them.
inline bool hardwareTestsAreAskedFor()
{
    return qEnvironmentVariableIntValue("ACHERON_HARDWARE_TESTS") > 0;
}

#define SKIP_UNLESS_HARDWARE_IS_ASKED_FOR() \
    if (!hardwareTestsAreAskedFor())        \
    QSKIP("needs this machine's own hardware, which ACHERON_HARDWARE_TESTS=1 asks for")
