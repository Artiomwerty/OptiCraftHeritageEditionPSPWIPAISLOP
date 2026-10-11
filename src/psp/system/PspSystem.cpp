#include "psp/system/PspSystem.h"

#include <pspkernel.h>
#include <psprtc.h>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

namespace
{
std::string g_appDir = "ms0:/PSP/GAME/OptiCraft";
}

namespace PspSystem
{
void setAppDirFromArgv0(const char *argv0)
{
    if (!argv0 || !*argv0)
    {
        chdir(g_appDir.c_str());
        return;
    }
    std::string s(argv0);
    const std::size_t slash = s.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
        g_appDir = s.substr(0, slash);
    // Some launchers hand over argv[0] without a device ("/PSP/GAME/OptiCraft/EBOOT.PBP"). libc then
    // has no current drive and any path without "xx0:" crashes inside __path_absolute, so always
    // work with a full "ms0:/..." directory and make it the current directory.
    if (g_appDir.find(':') == std::string::npos)
        g_appDir = std::string("ms0:") + (g_appDir.empty() || g_appDir[0] != '/' ? "/" : "") + g_appDir;
    chdir(g_appDir.c_str());
}

const std::string &appDir() { return g_appDir; }

uint64_t micros() { return static_cast<uint64_t>(sceKernelGetSystemTimeWide()); }
uint32_t millis() { return static_cast<uint32_t>(micros() / 1000u); }

void bootTrace(const char *fmt, ...)
{
    static int lines = 0;
    if (lines >= 320)
        return;
    ++lines;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::string path = g_appDir + "/boot.log";
    FILE *f = fopen(path.c_str(), lines == 1 ? "w" : "a");
    if (!f)
        return;
    fprintf(f, "%8u ms  %s", (unsigned)millis(), msg);
    fclose(f);
}

void exitGame() { sceKernelExitGame(); }
}
