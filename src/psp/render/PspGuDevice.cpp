#include "psp/render/PspGuDevice.h"

#include <malloc.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>
#include <psputility.h>

#include <cstring>
#include <string>

#include "psp/PspFramePacer.h"
#include "psp/system/PspSystem.h"

namespace
{
unsigned int __attribute__((aligned(16))) g_list[262144]; // 1 MB command list
constexpr std::size_t kArenaBytes = 1536 * 1024;
unsigned char *g_arena[2] = {nullptr, nullptr};
int g_arenaIndex = 0;
std::size_t g_arenaUsed = 0;
bool g_init = false;
PspFramePacer g_pacer(2);
PspGu::Prof g_prof;
PspGu::Stats g_stats;
std::size_t g_vramNext = 0;
}

extern void pspRenderInvalidateState();

namespace
{
void utf8ToUtf16(const std::string &in, unsigned short *out, std::size_t cap)
{
    std::size_t o = 0;
    for (std::size_t i = 0; i < in.size() && o + 1 < cap;)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        unsigned cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        ++i;
        while (extra-- > 0 && i < in.size())
            cp = (cp << 6) | (static_cast<unsigned char>(in[i++]) & 0x3F);
        out[o++] = static_cast<unsigned short>(cp > 0xFFFF ? '?' : cp);
    }
    out[o] = 0;
}

std::string utf16ToUtf8(const unsigned short *in)
{
    std::string r;
    for (; *in; ++in)
    {
        const unsigned cp = *in;
        if (cp < 0x80) r += static_cast<char>(cp);
        else if (cp < 0x800) { r += static_cast<char>(0xC0 | (cp >> 6)); r += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { r += static_cast<char>(0xE0 | (cp >> 12)); r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); r += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    return r;
}

// Everything init() programs that the renderer's own state cache does not re-send.
void applyBaseGeState()
{
    sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void *>(0), PspGu::kBufferWidth);
    sceGuDispBuffer(PspGu::kScreenWidth, PspGu::kScreenHeight, reinterpret_cast<void *>(PspGu::kFrameBufferBytes), PspGu::kBufferWidth);
    sceGuDepthBuffer(reinterpret_cast<void *>(PspGu::kFrameBufferBytes * 2), PspGu::kBufferWidth);
    sceGuOffset(2048 - (PspGu::kScreenWidth / 2), 2048 - (PspGu::kScreenHeight / 2));
    sceGuViewport(2048, 2048, PspGu::kScreenWidth, PspGu::kScreenHeight);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, PspGu::kScreenWidth, PspGu::kScreenHeight);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    ScePspFMatrix4 id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    sceGuSetMatrix(GU_VIEW, &id);
    sceGuSetMatrix(GU_MODEL, &id);
    sceGuSetMatrix(GU_PROJECTION, &id);
}
} // namespace

namespace PspGu
{
bool oskEdit(const std::string &initial, const char *desc, int limit, std::string &out)
{
    if (!g_init)
        return false;
    static unsigned short sDesc[64], sIn[260], sOut[260];
    std::memset(sOut, 0, sizeof(sOut));
    utf8ToUtf16(desc ? desc : "", sDesc, 64);
    utf8ToUtf16(initial, sIn, 260);
    if (limit < 1) limit = 32;
    if (limit > 250) limit = 250;

    SceUtilityOskData data;
    std::memset(&data, 0, sizeof(data));
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines = 1;
    data.unk_24 = 1;
    data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc = sDesc;
    data.intext = sIn;
    data.outtextlength = limit;
    data.outtextlimit = limit;
    data.outtext = sOut;

    SceUtilityOskParams params;
    std::memset(&params, 0, sizeof(params));
    params.base.size = sizeof(params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &params.base.buttonSwap);
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.datacount = 1;
    params.data = &data;

    // Close the frame in progress; the system dialog draws over a plain background.
    sceGuFinish();
    sceGuSync(0, 0);
    const int initRc = sceUtilityOskInitStart(&params);
    PspSystem::bootTrace("osk: init -> 0x%08x\n", (unsigned)initRc);
    bool done = initRc < 0;
    int guard = 0, lastStatus = -1, visibleSeen = 0;
    const std::uint64_t startUs = PspSystem::micros();
    while (!done && guard++ < 60 * 60 * 10)
    {
        sceGuStart(GU_DIRECT, g_list);
        applyBaseGeState();
        sceGuClearColor(0xFF2A1E14);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(0, 0);
        const int status = sceUtilityOskGetStatus();
        if (status != lastStatus)
        {
            lastStatus = status;
            PspSystem::bootTrace("osk: status %d\n", status);
        }
        switch (status)
        {
        case PSP_UTILITY_OSK_DIALOG_VISIBLE: visibleSeen = 1; sceUtilityOskUpdate(1); break;
        case PSP_UTILITY_OSK_DIALOG_QUIT: sceUtilityOskShutdownStart(); break;
        case PSP_UTILITY_OSK_DIALOG_NONE: done = true; break;
        default: break;
        }
        // Never trap the player: if the dialog has not shown up after 4 s, give up.
        if (!visibleSeen && PspSystem::micros() - startUs > 4000000ull)
        {
            PspSystem::bootTrace("osk: never became visible (status %d), giving up\n", status);
            sceUtilityOskShutdownStart();
            for (int i = 0; i < 120 && sceUtilityOskGetStatus() != PSP_UTILITY_OSK_DIALOG_NONE; ++i)
            {
                if (sceUtilityOskGetStatus() == PSP_UTILITY_OSK_DIALOG_QUIT) sceUtilityOskShutdownStart();
                sceDisplayWaitVblankStart();
            }
            done = true;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    const bool changed = initRc >= 0 && data.result == PSP_UTILITY_OSK_RESULT_CHANGED;
    if (changed)
        out = utf16ToUtf8(sOut);
    PspSystem::bootTrace("osk: done result=%d\n", data.result);

    // Back to the game: re-send GE state and open the next list.
    sceGuStart(GU_DIRECT, g_list);
    applyBaseGeState();
    sceGuDisable(GU_DEPTH_TEST);
    pspRenderInvalidateState();
    g_pacer = PspFramePacer(2);
    return changed;
}

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
    sceGuEnable(GU_CLIP_PLANES);
    {
        // World/view matrices are not guaranteed to start as identity; the renderer only ever sets
        // the "model" and projection ones, so pin the others.
        ScePspFMatrix4 id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        sceGuSetMatrix(GU_VIEW, &id);
        sceGuSetMatrix(GU_MODEL, &id);
        sceGuSetMatrix(GU_PROJECTION, &id);
    }
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
    // Frame profile for boot.log: how long the CPU took to build the frame vs how long the GE
    // then needed to draw it (the two are not overlapped yet).
    static std::uint64_t lastEnd = 0, geSum = 0, frameSum = 0, workSum = 0, lastReturn = 0;
    static unsigned profFrames = 0, profLines = 0;
    const std::uint64_t t0 = PspSystem::micros();
    if (lastReturn) workSum += t0 - lastReturn;
    sceGuFinish(); // submit: the GE starts drawing now, in parallel with the pacing wait below
    const std::uint64_t t1 = PspSystem::micros();
    if (lastEnd) frameSum += t1 - lastEnd;
    lastEnd = t1;
    if (++profFrames == 120)
    {
        if (profLines++ < 40)
            PspSystem::bootTrace("perf: %u us/frame (%.1f fps), work %u us (tick %u light %u render %u display %u), GE+wait %u us, draws %u verts %u, screen open %u/120 (world %u overlay %u clear %u screen %u)",
                                 (unsigned)(frameSum / 119), 119.0e6 / (double)(frameSum ? frameSum : 1),
                                 (unsigned)(workSum / 120), g_prof.tick / 120, g_prof.light / 120, g_prof.render / 120,
                                 g_prof.display / 120, (unsigned)(geSum / 120),
                                 g_stats.lastDrawCalls, g_stats.lastVertices, g_prof.gui, g_prof.world / 120, g_prof.overlay / 120, g_prof.clear / 120, g_prof.screen / 120);
            g_prof = Prof();
            workSum = 0;
        profFrames = 0; geSum = 0; frameSum = 0;
    }

    unsigned waits = g_pacer.waitsNeeded(sceDisplayGetVcount());
    while (waits--)
        sceDisplayWaitVblankStart();
    PspSystem::setStage("present: waiting for GE");
    sceGuSync(0, 0); // the frame must be complete before it is flipped to the screen
    PspSystem::heartbeat();
    PspSystem::setStage("after present");
    geSum += PspSystem::micros() - t1;
    g_pacer.presented(sceDisplayGetVcount());
    sceGuSwapBuffers();
    lastReturn = PspSystem::micros();

    ++g_stats.frames;
    if (g_stats.frames == 1 || g_stats.frames == 2 || g_stats.frames == 10 || g_stats.frames == 100 ||
        g_stats.frames == 1000)
        PspSystem::bootTrace("present: frame %u (draws %u, verts %u, slow %u)\n", g_stats.frames,
                             g_stats.lastDrawCalls, g_stats.lastVertices, g_stats.slowFrames);
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
Prof &prof() { return g_prof; }

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
