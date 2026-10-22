#pragma once
// PSP Graphics Engine device: initialisation, the per-frame command list, a per-frame vertex
// arena, and presenting at a locked 30 fps. Everything that touches sceGu lives behind this and
// RenderAPI_GU_PSP.cpp.
#include <cstddef>
#include <cstdint>
#include <string>

namespace PspGu
{
// Face culling is normally forced off on the GE; models that rely on it (the zero-thickness book
// pages, whose front and back faces coincide) switch it on for their draw.
inline bool g_honorCull = false;
// Rendering experiments, read from render.ini beside the EBOOT (all off = normal behaviour):
//   noindex=1 draw quads as plain triangle lists, texsync=1 flush the texture cache before every draw,
//   flush=1 write back the whole data cache each frame, noclip=1 leave GE clip planes off.
// render.ini: skip=bitmask of passes to leave out (1 sky, 2 entities, 4 particles, 8 block cracks, 16 translucent
// terrain, 32 rain, 64 clouds, 128 hand, 256 opaque terrain); near=<float> camera near plane.
inline unsigned g_dbgSkip = 0;
inline bool g_dbgDefer = false; // render.ini defer=1: free vertex/texture memory only after the frame (test96; caused flicker)
inline float g_nearPlane = 0.3f;
inline bool g_dbgNoIndex = true, g_dbgTexSync = true, g_dbgFlushAll = false, g_dbgNoClip = false;
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
// Frees vertex/texture memory that was released while the frame was being recorded (see RenderAPI_GU_PSP.cpp).
void releaseDeferred();

// Finishes the current list, waits for the GPU, waits for the pacer's vblank, flips, and starts
// the next frame's list. Call once per game frame.
void present();

// Per-frame scratch memory the GE can read (16-byte aligned, valid until the next present()).
// Returns nullptr when the arena is exhausted (the caller drops the draw).
void *frameAlloc(std::size_t bytes);
// Writes the arena bytes allocated since the last call out of the CPU data cache. The GE starts drawing
// each primitive as soon as sceGuDrawArray is issued (GU_DIRECT advances the stall address), so this
// must run right before every draw that reads arena memory, not only at present().
void flushArena();

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
struct Prof { unsigned tick = 0, light = 0, render = 0, display = 0, gui = 0, world = 0, overlay = 0, clear = 0, screen = 0; };
Prof &prof();
void countDraw(unsigned vertices);

// VRAM suballocator for textures (bump allocation, freed all at once on reset).
// Returns the byte offset from the start of VRAM or -1 when full.
int vramAlloc(std::size_t bytes);
void vramReset();
std::size_t vramFree();
}
