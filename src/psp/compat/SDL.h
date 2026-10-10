#pragma once
// Minimal SDL stand-in for the PSP build.
//
// The shared engine code (PlatformCompat.h, GuiScreen, Config, ...) calls a handful of SDL
// functions for time, sleeping, clipboard and the cursor. The PSP has no SDL window, so this
// header supplies just those few functions on top of PSP system calls and keeps the shared
// code unmodified. Anything that needs a real SDL window or event pump lives in src/pc and is
// not part of the PSP build.
#include <cstdint>
#include <cstdlib>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int32_t Sint32;

struct SDL_Window;
typedef void *SDL_GLContext;

struct SDL_Event { Uint32 type; };

enum SDL_ThreadPriority { SDL_THREAD_PRIORITY_LOW, SDL_THREAD_PRIORITY_NORMAL, SDL_THREAD_PRIORITY_HIGH };

Uint32 SDL_GetTicks();
void SDL_Delay(Uint32 ms);
int SDL_SetThreadPriority(SDL_ThreadPriority priority);
Uint32 SDL_GetMouseState(int *x, int *y);
int SDL_OpenURL(const char *url);
char *SDL_GetClipboardText();
int SDL_SetClipboardText(const char *text);
void SDL_free(void *p);
const char *SDL_GetError();
void *SDL_GL_GetProcAddress(const char *name);
