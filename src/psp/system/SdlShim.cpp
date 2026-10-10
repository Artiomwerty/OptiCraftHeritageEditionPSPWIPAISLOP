// SdlShim.cpp — the few SDL entry points shared engine code calls, on PSP system calls.
#ifdef PSP_PLATFORM
#include "SDL.h"
#include "psp/system/PspSystem.h"
#include <pspkernel.h>
#include <cstdlib>

Uint32 SDL_GetTicks() { return PspSystem::millis(); }
void SDL_Delay(Uint32 ms) { sceKernelDelayThread(ms * 1000); }
int SDL_SetThreadPriority(SDL_ThreadPriority) { return 0; }
Uint32 SDL_GetMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
int SDL_OpenURL(const char *) { return -1; }
char *SDL_GetClipboardText() { return nullptr; }
int SDL_SetClipboardText(const char *) { return -1; }
void SDL_free(void *p) { std::free(p); }
const char *SDL_GetError() { return ""; }
void *SDL_GL_GetProcAddress(const char *) { return nullptr; }
#endif
