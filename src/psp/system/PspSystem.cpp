#include "psp/system/PspSystem.h"

#include <pspkernel.h>
#include <psprtc.h>

namespace
{
std::string g_appDir = "ms0:/PSP/GAME/OptiCraft";
}

namespace PspSystem
{
void setAppDirFromArgv0(const char *argv0)
{
    if (!argv0 || !*argv0)
        return;
    std::string s(argv0);
    const std::size_t slash = s.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
        g_appDir = s.substr(0, slash);
}

const std::string &appDir() { return g_appDir; }

uint64_t micros() { return static_cast<uint64_t>(sceKernelGetSystemTimeWide()); }
uint32_t millis() { return static_cast<uint32_t>(micros() / 1000u); }

void exitGame() { sceKernelExitGame(); }
}
