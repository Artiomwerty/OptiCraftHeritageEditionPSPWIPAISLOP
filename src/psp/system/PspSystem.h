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

// Hang diagnostics: stage names + frame heartbeat; a watchdog thread logs once if no frame completes for 4 s.
void setStage(const char *s);
void heartbeat();
void startWatchdog();

// Leaves to the XMB. Safe to call from any thread.
void exitGame();
}
