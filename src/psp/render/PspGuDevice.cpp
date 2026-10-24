#include "psp/render/PspGuDevice.h"

#include <malloc.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>
#include <psputility.h>

#include <cstring>
#include <string>

#include "psp/PspFramePacer.h"
#include "psp/system/PspSystem.h"
namespace PspProfile { void dump(); }
extern volatile unsigned g_pspAudioBlocks;
extern volatile int g_pspAudioLastError;
extern volatile int g_pspAudioVoices;

namespace
{
unsigned int __attribute__((aligned(16))) g_list[262144]; // 1 MB command list
constexpr std::size_t kArenaBytes = 1536 * 1024;
unsigned char *g_arena[2] = {nullptr, nullptr};
int g_arenaIndex = 0;
std::size_t g_arenaUsed = 0;
std::size_t g_arenaFlushed = 0;
bool g_init = false;
PspFramePacer g_pacer(2);
PspGu::Prof g_prof;
PspGu::Stats g_stats;
std::size_t g_vramNext = 0;
// Presentation diagnostics (boot.log "frames:" line).
struct FrameDiag { unsigned empty = 0, emptyRun = 0, maxEmptyRun = 0, startFail = 0, syncErr = 0, dispMismatch = 0, sameBuf = 0, flipWaits = 0, lateFlips = 0, geMissed = 0; };
unsigned g_sentinelCounter = 1;
FrameDiag g_diag;
bool g_flipPending = false;
unsigned g_flipVcount = 0;
struct CallerDiag { void *ra = nullptr; unsigned frames = 0, empty = 0; };
CallerDiag g_callers[8];

int noteCaller(bool empty)
{
    void *ra = PspGu::g_presentCaller;
    PspGu::g_presentCaller = nullptr;
    for (int i = 0; i < 8; ++i)
    {
        CallerDiag &c = g_callers[i];
        if (c.ra == ra || c.ra == nullptr)
        {
            c.ra = ra;
            ++c.frames;
            if (empty) ++c.empty;
            return i;
        }
    }
    return -1;
}

// The last 16 presents, written to boot.log on a screenshot (L+R+SELECT) or a HANG, so the moment an
// old picture shows up can be inspected.
struct PresentRec { unsigned t; signed char caller; unsigned char empty; unsigned short draws; unsigned verts; unsigned fb; int geState; };
PresentRec g_presents[16];
unsigned g_presentPos = 0;

int callerIndex(void *ra)
{
    for (int i = 0; i < 8; ++i)
        if (g_callers[i].ra == ra) return i;
    return -1;
}

void dumpPresents(const char *why)
{
    PspSystem::bootTrace("last presents (%s), newest first: [ms caller empty draws verts fb ge]\n", why);
    char line[260];
    int n = 0;
    for (unsigned k = 0; k < 16 && k < g_presentPos; ++k)
    {
        const PresentRec &p = g_presents[(g_presentPos - 1 - k) & 15];
        n += std::snprintf(line + n, sizeof(line) - n, " [%u c%d %s d%u v%u %06x g%d]", p.t, (int)p.caller,
                           p.empty ? "E" : "-", (unsigned)p.draws, p.verts, p.fb, p.geState);
        if (n > 200 || k == 15 || k + 1 >= g_presentPos)
        {
            PspSystem::bootTrace("%s\n", line);
            n = 0;
        }
    }
}

void hangHook() { dumpPresents("hang"); }

void dumpCallers()
{
    char line[260];
    int n = std::snprintf(line, sizeof(line), "present callers (ra frames/empty):");
    for (const CallerDiag &c : g_callers)
    {
        if (!c.ra) break;
        n += std::snprintf(line + n, sizeof(line) - n, " %p %u/%u", c.ra, c.frames, c.empty);
        if (n > 220) break;
    }
    PspSystem::bootTrace("%s\n", line);
}
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
    if (!PspGu::g_dbgNoClip) sceGuEnable(GU_CLIP_PLANES);
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
        case 2: /* visible (real firmware numbering) */ visibleSeen = 1; sceUtilityOskUpdate(1); break;
        case 3: /* quit */ sceUtilityOskShutdownStart(); break;
        case 0: done = true; break;
        default: break;
        }
        // Never trap the player: if the dialog has not shown up after 4 s, give up.
        if (!visibleSeen && PspSystem::micros() - startUs > 4000000ull)
        {
            PspSystem::bootTrace("osk: never became visible (status %d), giving up\n", status);
            sceUtilityOskShutdownStart();
            for (int i = 0; i < 120 && sceUtilityOskGetStatus() != 0; ++i)
            {
                if (sceUtilityOskGetStatus() == 3) sceUtilityOskShutdownStart();
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

// render.ini: tiny "key=1" text file so rendering experiments need no rebuild.
static void loadRenderIni()
{
    char path[300];
    std::snprintf(path, sizeof(path), "%s/render.ini", PspSystem::appDir().c_str());
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    char buf[256];
    const int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    PspGu::g_dbgNoIndex = std::strstr(buf, "noindex=0") == nullptr;
    PspGu::g_dbgTexSync = std::strstr(buf, "texsync=0") == nullptr;
    PspGu::g_dbgFlushAll = std::strstr(buf, "flush=1") != nullptr;
    PspGu::g_dbgNoClip = std::strstr(buf, "noclip=1") != nullptr;
    PspGu::g_dbgDefer = std::strstr(buf, "defer=1") != nullptr;
    PspGu::g_skipEmpty = std::strstr(buf, "skipempty=0") == nullptr;
    PspGu::g_lateFlip = std::strstr(buf, "lateflip=1") != nullptr;
    if (const char *k = std::strstr(buf, "skip=")) PspGu::g_dbgSkip = static_cast<unsigned>(std::atoi(k + 5));
    if (const char *k = std::strstr(buf, "near=")) { const float v = static_cast<float>(std::atof(k + 5)); if (v >= 0.05f && v <= 4.0f) PspGu::g_nearPlane = v; }
    PspSystem::bootTrace("render.ini: lateflip=%d skipempty=%d defer=%d skip=%u near=%.2f noindex=%d texsync=%d flush=%d noclip=%d\n", PspGu::g_lateFlip, PspGu::g_skipEmpty, PspGu::g_dbgDefer, PspGu::g_dbgSkip, (double)PspGu::g_nearPlane, PspGu::g_dbgNoIndex, PspGu::g_dbgTexSync, PspGu::g_dbgFlushAll, PspGu::g_dbgNoClip);
}

bool init()
{
    if (g_init)
        return true;
    loadRenderIni();
    PspSystem::g_hangHook = hangHook;
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
    if (!g_dbgNoClip) sceGuEnable(GU_CLIP_PLANES);
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

// Screenshot: hold L + R + SELECT. Saves the frame just shown as shotNN.bmp next to the EBOOT (max 8 per
// run) so rendering glitches can be inspected exactly. Costs nothing unless the combo is held.
static void maybeScreenshot()
{
    static bool prevCombo = false;
    static int shots = 0;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0)
        return;
    const unsigned combo = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT;
    const bool now = (pad.Buttons & combo) == combo;
    const bool fire = now && !prevCombo;
    prevCombo = now;
    if (!fire || shots >= 8)
        return;
    dumpPresents("screenshot");
    void *top = nullptr; int bw = 0, fmt = 0;
    sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (!top || bw <= 0 || fmt != PSP_DISPLAY_PIXEL_FORMAT_8888)
        return;
    const unsigned *src = reinterpret_cast<const unsigned *>((reinterpret_cast<unsigned>(top) & 0x0FFFFFFFu) | 0x44000000u);
    const int w = 480, h = 272, rowBytes = w * 3, total = 54 + rowBytes * h;
    unsigned char *bmp = static_cast<unsigned char *>(malloc(total));
    if (!bmp)
        return;
    std::memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    *reinterpret_cast<unsigned *>(bmp + 2) = total;
    *reinterpret_cast<unsigned *>(bmp + 10) = 54;
    *reinterpret_cast<unsigned *>(bmp + 14) = 40;
    *reinterpret_cast<int *>(bmp + 18) = w;
    *reinterpret_cast<int *>(bmp + 22) = h;
    *reinterpret_cast<unsigned short *>(bmp + 26) = 1;
    *reinterpret_cast<unsigned short *>(bmp + 28) = 24;
    *reinterpret_cast<unsigned *>(bmp + 34) = rowBytes * h;
    for (int y = 0; y < h; ++y)
    {
        unsigned char *dst = bmp + 54 + (h - 1 - y) * rowBytes;
        const unsigned *row = src + y * bw;
        for (int x = 0; x < w; ++x)
        {
            const unsigned c = row[x]; // ABGR
            dst[x * 3 + 0] = (c >> 16) & 0xFF;
            dst[x * 3 + 1] = (c >> 8) & 0xFF;
            dst[x * 3 + 2] = c & 0xFF;
        }
    }
    char path[300];
    std::snprintf(path, sizeof(path), "%s/shot%02d.bmp", PspSystem::appDir().c_str(), ++shots);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0)
    {
        sceIoWrite(fd, bmp, total);
        sceIoClose(fd);
        PspSystem::bootTrace("screenshot: %s\n", path);
    }
    free(bmp);
}

static void startList()
{
    int rc = sceGuStart(GU_DIRECT, g_list);
    if (rc < 0)
    {
        ++g_diag.startFail;
        if (g_diag.startFail <= 5)
            PspSystem::bootTrace("present: sceGuStart failed 0x%08x\n", (unsigned)rc);
    }
}

extern "C" int ge_list_executed[]; // libpspgu: id of the list sceGuStart queued

// Waits for the GE; returns the list's state afterwards (0 = done).
static int syncList()
{
    const int rc = sceGuSync(0, 0);
    if (rc < 0)
    {
        ++g_diag.syncErr;
        if (g_diag.syncErr <= 5)
            PspSystem::bootTrace("present: sceGuSync error 0x%08x\n", (unsigned)rc);
    }
    return sceGeListSync(ge_list_executed[0], 1);
}

static void recordPresent(int caller, bool empty, unsigned fb, int geState)
{
    PresentRec &p = g_presents[g_presentPos++ & 15];
    p.t = PspSystem::millis();
    p.caller = static_cast<signed char>(caller);
    p.empty = empty ? 1 : 0;
    p.draws = static_cast<unsigned short>(g_stats.drawCalls > 65535 ? 65535 : g_stats.drawCalls);
    p.verts = g_stats.vertices;
    p.fb = fb;
    p.geState = geState;
}

void present()
{
    if (!g_init)
        return;

    // A frame that drew and cleared nothing would flip a back buffer still holding a picture from two
    // frames ago (the world list flashing over the loading screen). Run the list, but keep showing the
    // current front buffer.
    const int caller = noteCaller(g_stats.drawCalls == 0);
    if (g_stats.drawCalls == 0 && g_skipEmpty)
    {
        flushArena();
        sceGuFinish();
        const int ge = syncList();
        recordPresent(caller, true, 0xFFFFFF, ge);
        releaseDeferred();
        g_arenaIndex ^= 1;
        g_arenaUsed = 0;
        g_arenaFlushed = 0;
        PspSystem::heartbeat();
        ++g_diag.empty;
        if (++g_diag.emptyRun == 30)
            PspSystem::bootTrace("present: 30 empty frames in a row (nothing drawn, screen held)\n");
        startList();
        return;
    }
    if (g_diag.emptyRun > g_diag.maxEmptyRun) g_diag.maxEmptyRun = g_diag.emptyRun;
    g_diag.emptyRun = 0;

    // Make the CPU-written vertex data visible to the GE, then run the frame.
    if (g_dbgFlushAll) sceKernelDcacheWritebackAll(); else flushArena();
    // Frame profile for boot.log: how long the CPU took to build the frame vs how long the GE
    // then needed to draw it (the two are not overlapped yet).
    static std::uint64_t lastEnd = 0, geSum = 0, frameSum = 0, workSum = 0, lastReturn = 0;
    static unsigned profFrames = 0, profLines = 0;
    const std::uint64_t t0 = PspSystem::micros();
    if (lastReturn) workSum += t0 - lastReturn;
    // Sentinel: one pixel in the bottom-right corner carrying the frame number. Read back after the GE
    // is done: if it is not there, the GE did not draw this frame into the buffer about to be shown.
    const unsigned sentinel = 0xFF000000u | ((g_sentinelCounter++ * 2654435761u) & 0x00FFFFFFu);
    {
        struct V { unsigned color; short x, y, z; };
        V *v = static_cast<V *>(sceGuGetMemory(2 * sizeof(V)));
        v[0] = {sentinel, short(kScreenWidth - 1), short(kScreenHeight - 1), 0};
        v[1] = {sentinel, short(kScreenWidth), short(kScreenHeight), 0};
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_BLEND);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_LIGHTING);
        sceGuPixelMask(0);
        sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
        pspRenderInvalidateState();
    }
    sceGuFinish(); // submit: the GE starts drawing now, in parallel with the pacing wait below
    const std::uint64_t t1 = PspSystem::micros();
    if (lastEnd) frameSum += t1 - lastEnd;
    lastEnd = t1;
    if (++profFrames == 120)
    {
        if (profLines++ < 90)
            PspSystem::bootTrace("perf: %u us/frame (%.1f fps), work %u us (tick %u light %u render %u display %u), GE+wait %u us, draws %u verts %u, screen open %u/120 (world %u overlay %u clear %u screen %u)",
                                 (unsigned)(frameSum / 119), 119.0e6 / (double)(frameSum ? frameSum : 1),
                                 (unsigned)(workSum / 120), g_prof.tick / 120, g_prof.light / 120, g_prof.render / 120,
                                 g_prof.display / 120, (unsigned)(geSum / 120),
                                 g_stats.lastDrawCalls, g_stats.lastVertices, g_prof.gui, g_prof.world / 120, g_prof.overlay / 120, g_prof.clear / 120, g_prof.screen / 120);
            { const struct mallinfo mi = mallinfo(); PspSystem::bootTrace("mem: heap used %u KB, arena %u KB, arenaDrops %u, audio blocks %u voices %d err %d\n", (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024), (unsigned)g_stats.arenaDrops, (unsigned)g_pspAudioBlocks, (int)g_pspAudioVoices, (int)g_pspAudioLastError); }
            PspSystem::bootTrace("frames: empty %u (longest run %u), startFail %u, syncErr %u, dispMismatch %u, sameBuf %u, lateFlips %u flipWaits %u, geMissed %u\n", g_diag.empty, g_diag.maxEmptyRun, g_diag.startFail, g_diag.syncErr, g_diag.dispMismatch, g_diag.sameBuf, g_diag.lateFlips, g_diag.flipWaits, g_diag.geMissed);
            dumpCallers();
            PspProfile::dump();
            g_prof = Prof();
            workSum = 0;
        profFrames = 0; geSum = 0; frameSum = 0;
    }

    // On time: wait for the 30 fps slot as before. Late (most frames in the world): flip now and only
    // wait for the vblank if the GE is about to draw into the old front buffer before it happened
    // (waitFlip, called before the next frame's first draw). The game tick runs in between.
    const unsigned vcNow = sceDisplayGetVcount();
    const bool lateFlip = g_lateFlip && g_pacer.late(vcNow);
    unsigned waits = lateFlip ? 0u : g_pacer.waitsNeeded(vcNow);
    while (waits--)
        sceDisplayWaitVblankStart();
    const int geState = syncList(); // the frame must be complete before it is flipped to the screen
    releaseDeferred();
    geSum += PspSystem::micros() - t1;
    g_pacer.presented(sceDisplayGetVcount());
    {
        // Check that the display really points at the buffer just drawn and the GE moves on to the other.
        const unsigned newDraw = reinterpret_cast<unsigned>(sceGuSwapBuffers()) & 0x1FFFFFu;
        void *top = nullptr; int bw = 0, fmt = 0;
        sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
        const unsigned disp = reinterpret_cast<unsigned>(top) & 0x1FFFFFu;
        recordPresent(caller, false, disp, geState);
        {
            const unsigned got = *reinterpret_cast<volatile unsigned *>(0x44000000u + disp +
                                     ((kScreenHeight - 1) * kBufferWidth + (kScreenWidth - 1)) * 4u);
            if (got != sentinel)
            {
                ++g_diag.geMissed;
                if (g_diag.geMissed <= 8)
                    PspSystem::bootTrace("present: GE did not draw frame into %06x (pixel %08x, want %08x, draws %u, ge %d)\n",
                                         disp, got, sentinel, g_stats.drawCalls, geState);
            }
        }
        if (lateFlip)
        {
            ++g_diag.lateFlips;
            g_flipPending = true;
            g_flipVcount = sceDisplayGetVcount();
        }
        if (disp == newDraw)
        {
            ++g_diag.sameBuf;
            if (g_diag.sameBuf <= 5) PspSystem::bootTrace("present: display and draw buffer are the same (%06x)\n", disp);
        }
        else if (disp != 0 && disp != kFrameBufferBytes)
        {
            ++g_diag.dispMismatch;
            if (g_diag.dispMismatch <= 5) PspSystem::bootTrace("present: display shows unexpected buffer %06x (draw %06x)\n", disp, newDraw);
        }
    }
    maybeScreenshot();
    PspSystem::heartbeat();
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
    g_arenaFlushed = 0;
    startList();
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

void waitFlip()
{
    if (!g_flipPending)
        return;
    g_flipPending = false;
    // The display switches buffers at the next vblank; until then the buffer the GE is about to clear
    // is still on screen.
    if (sceDisplayGetVcount() == g_flipVcount)
    {
        ++g_diag.flipWaits;
        sceDisplayWaitVblankStart();
    }
}

void flushArena()
{
    if (g_arenaUsed <= g_arenaFlushed)
        return;
    sceKernelDcacheWritebackRange(g_arena[g_arenaIndex] + g_arenaFlushed, g_arenaUsed - g_arenaFlushed);
    g_arenaFlushed = g_arenaUsed;
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
