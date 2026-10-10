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

// Leaves to the XMB. Safe to call from any thread.
void exitGame();
}
