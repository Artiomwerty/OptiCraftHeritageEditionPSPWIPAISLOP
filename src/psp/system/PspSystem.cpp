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
#include <pspthreadman.h>
#include <malloc.h>

namespace {
struct CrumbEntry { const char *tag; int a; int b; unsigned t; };
CrumbEntry g_ring[16];
volatile unsigned g_ringPos = 0;
volatile unsigned g_beat = 0;
}

void crumb(const char *tag, int a, int b)
{
    const unsigned i = g_ringPos++;
    CrumbEntry &e = g_ring[i & 15];
    e.tag = tag; e.a = a; e.b = b; e.t = (unsigned)millis();
}

void heartbeat() { ++g_beat; }

static int dumpRing(char *out, int cap)
{
    int n = 0;
    const unsigned pos = g_ringPos;
    for (unsigned k = 0; k < 16 && k < pos; ++k)
    {
        const CrumbEntry &e = g_ring[(pos - 1 - k) & 15];
        if (e.tag == nullptr) continue;
        const int w = snprintf(out + n, cap - n, " [%u %s %d,%d]", e.t, e.tag, e.a, e.b);
        if (w <= 0 || w >= cap - n) break;
        n += w;
    }
    return n;
}

static int watchdogMain(SceSize, void *)
{
    unsigned last = g_beat;
    int stalled = 0, reports = 0;
    for (;;)
    {
        sceKernelDelayThread(1000 * 1000);
        const unsigned now = g_beat;
        if (now != last) { last = now; stalled = 0; continue; }
        ++stalled;
        if (stalled >= 5 && (stalled - 5) % 10 == 0 && reports < 4)
        {
            ++reports;
            const struct mallinfo mi = mallinfo();
            char text[700];
            int n = snprintf(text, sizeof(text), "%8u ms  HANG: no frame for %d s, heap used %u KB, arena %u KB, last:",
                (unsigned)millis(), stalled, (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024));
            n += dumpRing(text + n, (int)sizeof(text) - n - 2);
            text[n++] = '\n';
            char path[300];
            snprintf(path, sizeof(path), "%s/boot.log", g_appDir.empty() ? "ms0:/PSP/GAME/OptiCraft" : g_appDir.c_str());
            rawWrite(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, text, n);
        }
    }
    return 0;
}

void startWatchdog()
{
    const int th = sceKernelCreateThread("watchdog", watchdogMain, 0x70, 0x1000, PSP_THREAD_ATTR_USER, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

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
    {
        char r[500];
        int m = snprintf(r, sizeof(r), "             last:");
        m += dumpRing(r + m, (int)sizeof(r) - m - 2);
        r[m++] = '\n';
        rawWrite(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, r, m);
    }
    pspDebugScreenInit();
    pspDebugScreenPrintf("%s\nCrash logged to boot.log. Press any button/wait to exit.", text);
    sceKernelDelayThread(8 * 1000 * 1000);
    sceKernelExitGame();
}
void installCrashHandler() { pspDebugInstallErrorHandler(crashHandler); }

void exitGame() { sceKernelExitGame(); }
}
