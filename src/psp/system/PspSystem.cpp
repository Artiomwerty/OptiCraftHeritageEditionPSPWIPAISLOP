#include "psp/system/PspSystem.h"

#include <pspkernel.h>
#include <psprtc.h>
#include <cstdarg>
#include <pspiofilemgr.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

extern "C" char __cwd[256]; // newlib/pspsdk libcglue's current directory

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
    int rc = chdir(g_appDir.c_str());
    if (rc != 0)
    {
        // chdir() is refused on some launches and leaves libc without a current directory, which makes
        // every path starting with '/' crash in __path_absolute. Set it directly.
        strncpy(__cwd, g_appDir.c_str(), sizeof(__cwd) - 1);
        __cwd[sizeof(__cwd) - 1] = 0;
    }
    char cwd[256] = "";
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    bootTrace("setAppDir: chdir('%s')=%d cwd='%s'\n", g_appDir.c_str(), rc, cwd);
}

const std::string &appDir() { return g_appDir; }

uint64_t micros() { return static_cast<uint64_t>(sceKernelGetSystemTimeWide()); }
uint32_t millis() { return static_cast<uint32_t>(micros() / 1000u); }

// Raw sceIo logging: no libc path handling, so it works even when the C library has no usable
// current directory (which is what crashes fopen/stat on some launches).
static void rawWrite(const char *path, int flags, const char *text, int len)
{
    SceUID fd = sceIoOpen(path, flags, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, text, len);
    sceIoClose(fd);
}

void bootTrace(const char *fmt, ...)
{
    static int lines = 0;
    if (lines >= 1500)
        return;
    ++lines;
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[340];
    const int n = snprintf(line, sizeof(line), "%8u ms  %s", (unsigned)millis(), msg);
    if (n <= 0)
        return;
    const int len = n < (int)sizeof(line) ? n : (int)sizeof(line) - 1;
    char path[300];
    snprintf(path, sizeof(path), "%s/boot.log", g_appDir.empty() ? "ms0:/PSP/GAME/OptiCraft" : g_appDir.c_str());
    rawWrite(path, lines == 1 ? (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC) : (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND), line, len);
}

#include <pspdebug.h>
static void crashHandler(PspDebugRegBlock *r)
{
    char text[600];
    int n = snprintf(text, sizeof(text),
        "%8u ms  *** CRASH *** epc=%08lx ra=%08lx badva=%08lx cause=%08lx(code %lu) sp=%08lx a0-a3=%08lx %08lx %08lx %08lx v0=%08lx v1=%08lx\n",
        (unsigned)millis(), (unsigned long)r->epc, (unsigned long)r->r[31], (unsigned long)r->badvaddr, (unsigned long)r->cause,
        (unsigned long)((r->cause >> 2) & 0x1F), (unsigned long)r->r[29], (unsigned long)r->r[4], (unsigned long)r->r[5],
        (unsigned long)r->r[6], (unsigned long)r->r[7], (unsigned long)r->r[2], (unsigned long)r->r[3]);
    char path[300];
    snprintf(path, sizeof(path), "%s/boot.log", g_appDir.empty() ? "ms0:/PSP/GAME/OptiCraft" : g_appDir.c_str());
    if (n > 0) rawWrite(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, text, n);
    pspDebugScreenInit();
    pspDebugScreenPrintf("%s\nCrash logged to boot.log. Press any button/wait to exit.", text);
    sceKernelDelayThread(8 * 1000 * 1000);
    sceKernelExitGame();
}
void installCrashHandler() { pspDebugInstallErrorHandler(crashHandler); }

static volatile const char *g_stage = "boot";
static volatile unsigned g_beat = 0;
void setStage(const char *s) { g_stage = s; }
void heartbeat() { ++g_beat; }
static int watchdogMain(SceSize, void *)
{
    unsigned last = g_beat;
    int stalled = 0;
    for (;;)
    {
        sceKernelDelayThread(1000 * 1000);
        if (g_beat != last) { last = g_beat; stalled = 0; continue; }
        if (++stalled == 4)
            bootTrace("WATCHDOG: no frame for 4 s, stage=%s freeMem=%u maxBlock=%u\n", (const char *)g_stage,
                      (unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize());
    }
    return 0;
}
void startWatchdog()
{
    SceUID t = sceKernelCreateThread("watchdog", watchdogMain, 0x10, 4096, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
}

void exitGame() { sceKernelExitGame(); }
}
