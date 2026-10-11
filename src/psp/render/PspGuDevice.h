#pragma once
// PSP Graphics Engine device: initialisation, the per-frame command list, a per-frame vertex
// arena, and presenting at a locked 30 fps. Everything that touches sceGu lives behind this and
// RenderAPI_GU_PSP.cpp.
#include <cstddef>
#include <cstdint>
#include <string>

namespace PspGu
{
constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;
constexpr int kBufferWidth = 512;
constexpr unsigned kFrameBufferBytes = kBufferWidth * kScreenHeight * 4; // 0x88000
// First VRAM byte free for textures: after 2 colour buffers (8888) and the 16-bit depth buffer.
constexpr unsigned kVramFreeStart = kFrameBufferBytes * 2 + kBufferWidth * kScreenHeight * 2;
constexpr unsigned kVramSize = 0x200000; // the PSP has 2 MB of VRAM; past that addresses wrap onto the frame buffers

// Brings up the GE, clears both buffers, starts the first frame's command list.
bool init();

// System on-screen keyboard (the XMB one). Blocks until it closes; returns true and fills `out`
// when the user confirmed a changed text. Must be called between frames (not mid-render).
bool oskEdit(const std::string &initial, const char *desc, int limit, std::string &out);
bool isInitialized();

// Finishes the current list, waits for the GPU, waits for the pacer's vblank, flips, and starts
// the next frame's list. Call once per game frame.
void present();

// Per-frame scratch memory the GE can read (16-byte aligned, valid until the next present()).
// Returns nullptr when the arena is exhausted (the caller drops the draw).
void *frameAlloc(std::size_t bytes);

// Frame statistics for the on-screen / log report.
struct Stats
{
    unsigned frames = 0;
    unsigned slowFrames = 0;
    unsigned arenaPeakBytes = 0;
    unsigned drawCalls = 0;
    unsigned lastDrawCalls = 0;
    unsigned lastVertices = 0;
    unsigned vertices = 0;
    unsigned arenaDrops = 0;
    unsigned slowInARow = 0;
};
const Stats &stats();

// Per-phase CPU microseconds since the last perf line (written by the game loop).
struct Prof { unsigned tick = 0, light = 0, render = 0, display = 0, gui = 0; };
Prof &prof();
void countDraw(unsigned vertices);

// VRAM suballocator for textures (bump allocation, freed all at once on reset).
// Returns the byte offset from the start of VRAM or -1 when full.
int vramAlloc(std::size_t bytes);
void vramReset();
std::size_t vramFree();
}
