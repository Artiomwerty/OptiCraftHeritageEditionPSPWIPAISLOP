#pragma once
// PSP system glue shared by the game: install directory, clock, exit.
#include <cstdint>
#include <string>

namespace PspSystem
{
// Directory the EBOOT was started from, e.g. "ef0:/PSP/GAME/OptiCraft" (no trailing slash).
void setAppDirFromArgv0(const char *argv0);
const std::string &appDir();

// Milliseconds / microseconds since boot, from the PSP's system timer (monotonic).
uint32_t millis();
uint64_t micros();

// Boot breadcrumbs: appends one line to <appDir>/boot.log, at most 120 lines per run, so a hang or
// black screen can be located. Tiny (a few KB per run); never used while playing.
void bootTrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Writes a CRASH line (epc/ra/badvaddr) to boot.log on a fatal exception.
void installCrashHandler();

// In-memory breadcrumb ring (no file I/O): remembers the last 16 events. Dumped by the crash handler
// and the hang watchdog. Cheap enough to call from hot paths.
void crumb(const char *tag, int a, int b);
// Called once per presented frame; the watchdog thread logs a HANG line if it stops advancing.
void heartbeat();
void startWatchdog();

// Leaves to the XMB. Safe to call from any thread.
void exitGame();
}
