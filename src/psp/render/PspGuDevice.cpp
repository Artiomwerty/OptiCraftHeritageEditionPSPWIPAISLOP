#include "psp/render/PspGuDevice.h"

#include <malloc.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include "psp/PspFramePacer.h"

namespace
{
unsigned int __attribute__((aligned(16))) g_list[262144]; // 1 MB command list
constexpr std::size_t kArenaBytes = 1536 * 1024;
unsigned char *g_arena[2] = {nullptr, nullptr};
int g_arenaIndex = 0;
std::size_t g_arenaUsed = 0;
bool g_init = false;
PspFramePacer g_pacer(2);
PspGu::Stats g_stats;
std::size_t g_vramNext = 0;
}

namespace PspGu
{
bool init()
{
    if (g_init)
        return true;
    for (int i = 0; i < 2; ++i)
    {
        g_arena[i] = static_cast<unsigned char *>(memalign(64, kArenaBytes));
        if (!g_arena[i])
            return false;
    }
    g_arenaIndex = 0;
    g_arenaUsed = 0;

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void *>(0), kBufferWidth);
    sceGuDispBuffer(kScreenWidth, kScreenHeight, reinterpret_cast<void *>(kFrameBufferBytes), kBufferWidth);
    sceGuDepthBuffer(reinterpret_cast<void *>(kFrameBufferBytes * 2), kBufferWidth);
    sceGuOffset(2048 - (kScreenWidth / 2), 2048 - (kScreenHeight / 2));
    sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    sceGuStart(GU_DIRECT, g_list);
    g_vramNext = kVramFreeStart;
    g_init = true;
    return true;
}

bool isInitialized() { return g_init; }

void present()
{
    if (!g_init)
        return;

    // Make the CPU-written vertex data visible to the GE, then run the frame.
    sceKernelDcacheWritebackRange(g_arena[g_arenaIndex], g_arenaUsed);
    sceGuFinish();
    sceGuSync(0, 0);

    unsigned waits = g_pacer.waitsNeeded(sceDisplayGetVcount());
    while (waits--)
        sceDisplayWaitVblankStart();
    g_pacer.presented(sceDisplayGetVcount());
    sceGuSwapBuffers();

    ++g_stats.frames;
    g_stats.slowFrames = g_pacer.slowFrames();
    g_stats.slowInARow = g_pacer.slowFramesInARow();
    if (g_arenaUsed > g_stats.arenaPeakBytes)
        g_stats.arenaPeakBytes = static_cast<unsigned>(g_arenaUsed);
    g_stats.lastDrawCalls = g_stats.drawCalls;
    g_stats.lastVertices = g_stats.vertices;
    g_stats.drawCalls = 0;
    g_stats.vertices = 0;

    g_arenaIndex ^= 1;
    g_arenaUsed = 0;
    sceGuStart(GU_DIRECT, g_list);
}

void *frameAlloc(std::size_t bytes)
{
    const std::size_t aligned = (bytes + 15u) & ~static_cast<std::size_t>(15u);
    if (g_arenaUsed + aligned > kArenaBytes)
    {
        ++g_stats.arenaDrops;
        return nullptr;
    }
    void *p = g_arena[g_arenaIndex] + g_arenaUsed;
    g_arenaUsed += aligned;
    return p;
}

const Stats &stats() { return g_stats; }

void countDraw(unsigned vertices)
{
    ++g_stats.drawCalls;
    g_stats.vertices += vertices;
}

int vramAlloc(std::size_t bytes)
{
    const std::size_t aligned = (bytes + 255u) & ~static_cast<std::size_t>(255u);
    if (g_vramNext + aligned > kVramSize)
        return -1;
    const int off = static_cast<int>(g_vramNext);
    g_vramNext += aligned;
    return off;
}

void vramReset() { g_vramNext = kVramFreeStart; }
std::size_t vramFree() { return kVramSize - g_vramNext; }
}
