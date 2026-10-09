// PspDataCheck.cpp — PSP checkpoint 1: storage + GPU + controls on real hardware.
//
// NOT the game. It proves, on a real console, the three layers every later
// milestone stands on:
//   1. storage:  the project's own AssetPak reader mounts assets.pak from the
//                install folder and returns entries (platform/storage/AssetPak).
//   2. GPU:      PNGs from the pak are decoded with stb_image and drawn with sceGu.
//   3. controls: PspControls (the real game layout) is fed the live pad and its
//                output is shown, with a tally of which actions have been seen.
// A text report is printed first and everything is also written to datacheck.log.
//
// Diagnostic build: the texture is drawn twice, by the CPU (left, straight into the
// framebuffer) and by the GPU (right, sceGu sprite), plus an untextured gradient quad
// as a GPU sanity check. Whichever is wrong tells us where a bug is. A crash handler
// writes the faulting address and the last stage reached to datacheck.log and the
// screen; every button press is logged.

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspgu.h>
#include <malloc.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform/storage/AssetPak.h"
#include "psp/PspFramePacer.h"
#include "psp/input/PspControls.h"
#include "stb_image.h"

PSP_MODULE_INFO("OptiCraftDataCheck", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

namespace
{

constexpr int SCR_W = 480;
constexpr int SCR_H = 272;
constexpr int BUF_W = 512;
constexpr unsigned FB_BYTES = BUF_W * SCR_H * 4;      // one 32-bit framebuffer, 0x88000
constexpr unsigned COLOR_YELLOW = 0xFF00FFFF;         // ABGR
constexpr unsigned COLOR_WHITE = 0xFFFFFFFF;
constexpr unsigned COLOR_RED = 0xFF4040FF;
constexpr unsigned COLOR_GREEN = 0xFF60FF60;

volatile bool g_running = true;
FILE *g_log = nullptr;

int exitCallback(int, int, void *) { g_running = false; return 0; }

int callbackThread(SceSize, void *)
{
	int cb = sceKernelCreateCallback("exit_callback", exitCallback, nullptr);
	sceKernelRegisterExitCallback(cb);
	sceKernelSleepThreadCB();
	return 0;
}

void setupExitCallback()
{
	int th = sceKernelCreateThread("exit_thread", callbackThread, 0x11, 0xFA0, 0, nullptr);
	if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

uint64_t nowMs() { return sceKernelGetSystemTimeWide() / 1000ull; }

char g_logPath[300] = {};
char g_stage[96] = "start";

// Records the last stage reached, so a crash report says where it happened.
void stage(const char *name)
{
	snprintf(g_stage, sizeof(g_stage), "%s", name);
	if (g_log) { fprintf(g_log, "STAGE   %s  (t=%llu ms)\n", name, static_cast<unsigned long long>(nowMs())); fflush(g_log); }
}

// Runs on an unhandled exception. Keeps to sceIo and the debug screen: the C heap
// and stdio may be the thing that crashed.
void crashHandler(PspDebugRegBlock *r)
{
	char text[1100];
	snprintf(text, sizeof(text),
	         "\n*** CRASH ***\nstage : %s\nepc   : %08lX\nra    : %08lX\nbadva : %08lX\ncause : %08lX (code %lu)\n"
	         "sp    : %08lX\na0-a3 : %08lX %08lX %08lX %08lX\nv0/v1 : %08lX %08lX\n"
	         "(look up epc/ra in OptiCraft.unstripped.elf with psp-addr2line)\n",
	         g_stage, static_cast<unsigned long>(r->epc), static_cast<unsigned long>(r->r[31]),
	         static_cast<unsigned long>(r->badvaddr), static_cast<unsigned long>(r->cause),
	         static_cast<unsigned long>((r->cause >> 2) & 0x1F), static_cast<unsigned long>(r->r[29]),
	         static_cast<unsigned long>(r->r[4]), static_cast<unsigned long>(r->r[5]),
	         static_cast<unsigned long>(r->r[6]), static_cast<unsigned long>(r->r[7]),
	         static_cast<unsigned long>(r->r[2]), static_cast<unsigned long>(r->r[3]));
	if (g_logPath[0])
	{
		SceUID fd = sceIoOpen(g_logPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
		if (fd >= 0) { sceIoWrite(fd, text, strlen(text)); sceIoClose(fd); }
	}
	pspDebugScreenInit();
	pspDebugScreenSetTextColor(0xFF4040FF);
	pspDebugScreenPrintf("%s", text);
	pspDebugScreenSetTextColor(0xFFFFFFFF);
	pspDebugScreenPrintf("\nPress any button to exit. Send me datacheck.log.\n");
	SceCtrlData pad;
	for (;;)
	{
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons) break;
		sceKernelDelayThread(20000);
	}
	sceKernelExitGame();
}

void say(unsigned color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void say(unsigned color, const char *fmt, ...)
{
	char buf[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	pspDebugScreenSetTextColor(color);
	pspDebugScreenPrintf("%s", buf);
	if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

// argv[0] is e.g. "ms0:/PSP/GAME/OptiCraft/EBOOT.PBP" (ef0: on a PSP Go).
void appDir(const char *argv0, char *out, size_t n)
{
	snprintf(out, n, "%s", argv0 && *argv0 ? argv0 : "ms0:/PSP/GAME/OptiCraft/EBOOT.PBP");
	char *slash = strrchr(out, '/');
	if (slash) *slash = 0; else snprintf(out, n, "ms0:/PSP/GAME/OptiCraft");
}

// ---------------------------------------------------------------- textures ---

struct Texture
{
	int w = 0, h = 0;            // image size
	int tw = 0, th = 0;          // power-of-two buffer size actually uploaded
	uint32_t *pixels = nullptr;  // RGBA8888, 16-byte aligned, tw*th
	uint32_t checksum = 0;       // sum of all RGBA bytes of the decoded image
	uint64_t loadMs = 0;         // read from pak + decode
	bool hasAlpha = false;
	char key[96] = {};
	bool synthetic = false;
};

int pow2AtLeast(int v)
{
	int p = 16;
	while (p < v) p <<= 1;
	return p;
}

void freeTexture(Texture &t)
{
	free(t.pixels);
	t = Texture();
}

// Pads w*h RGBA pixels into a power-of-two, 16-byte-aligned buffer (the GPU wants both).
bool padToTexture(Texture &t, const unsigned char *rgba)
{
	t.tw = pow2AtLeast(t.w);
	t.th = pow2AtLeast(t.h);
	if (t.tw > 512 || t.th > 512) return false;
	t.pixels = static_cast<uint32_t *>(memalign(16, static_cast<size_t>(t.tw) * t.th * 4));
	if (!t.pixels) return false;
	memset(t.pixels, 0, static_cast<size_t>(t.tw) * t.th * 4);
	uint32_t sum = 0;
	for (int y = 0; y < t.h; ++y)
	{
		const unsigned char *src = rgba + static_cast<size_t>(y) * t.w * 4;
		memcpy(t.pixels + static_cast<size_t>(y) * t.tw, src, static_cast<size_t>(t.w) * 4);
		for (int x = 0; x < t.w * 4; ++x) sum += src[x];
		for (int x = 3; x < t.w * 4; x += 4) if (src[x] != 255) t.hasAlpha = true;
	}
	t.checksum = sum;
	sceKernelDcacheWritebackAll(); // GE reads RAM directly; push the CPU cache out first
	return true;
}

bool loadFromPak(const char *key, Texture &t, const char **why)
{
	t = Texture();
	snprintf(t.key, sizeof(t.key), "%s", key);
	const uint64_t t0 = nowMs();
	unsigned size = 0;
	unsigned char *file = AssetPak::load(key, &size);
	if (!file) { *why = "not in pak / read failed"; return false; }
	int w = 0, h = 0, n = 0;
	unsigned char *rgba = stbi_load_from_memory(file, static_cast<int>(size), &w, &h, &n, 4);
	free(file);
	if (!rgba) { *why = "PNG decode failed"; return false; }
	t.w = w; t.h = h;
	const bool ok = padToTexture(t, rgba);
	stbi_image_free(rgba);
	t.loadMs = nowMs() - t0;
	if (!ok) { freeTexture(t); *why = "larger than 512 or out of memory"; return false; }
	return true;
}

// Built-in test pattern, so the GPU and controls can still be tested if the pak is missing.
bool makePattern(Texture &t)
{
	t = Texture();
	snprintf(t.key, sizeof(t.key), "(built-in test pattern - no pak)");
	t.w = 128; t.h = 128; t.synthetic = true;
	unsigned char *rgba = static_cast<unsigned char *>(malloc(128 * 128 * 4));
	if (!rgba) return false;
	for (int y = 0; y < 128; ++y)
		for (int x = 0; x < 128; ++x)
		{
			unsigned char *p = rgba + (y * 128 + x) * 4;
			const bool c = ((x >> 4) + (y >> 4)) & 1;
			p[0] = c ? 230 : 40; p[1] = x * 2; p[2] = y * 2; p[3] = 255;
		}
	const bool ok = padToTexture(t, rgba);
	free(rgba);
	return ok;
}

// ---------------------------------------------------------------- CPU path ---

// Draws the texture straight into the framebuffer being drawn (uncached VRAM), no GPU.
// RGBA bytes in memory == the PSM_8888 framebuffer layout, so words copy across as is.
void cpuBlit(const Texture &t, unsigned drawOffset, float cx, float cy, float zoom)
{
	uint32_t *fb = reinterpret_cast<uint32_t *>(0x44000000u + drawOffset);
	const float left = cx - t.w * zoom * 0.5f, top = cy - t.h * zoom * 0.5f;
	const float right = left + t.w * zoom, bottom = top + t.h * zoom;
	int y0 = static_cast<int>(std::ceil(top)), y1 = static_cast<int>(std::floor(bottom));
	int x0 = static_cast<int>(std::ceil(left)), x1 = static_cast<int>(std::floor(right));
	if (y0 < 0) y0 = 0;
	if (x0 < 0) x0 = 0;
	if (y1 > SCR_H) y1 = SCR_H;
	if (x1 > SCR_W) x1 = SCR_W;
	const float inv = 1.0f / zoom;
	for (int y = y0; y < y1; ++y)
	{
		int sy = static_cast<int>((y + 0.5f - top) * inv);
		if (sy < 0) sy = 0;
		if (sy >= t.h) sy = t.h - 1;
		const uint32_t *srcRow = t.pixels + static_cast<size_t>(sy) * t.tw;
		uint32_t *dst = fb + static_cast<size_t>(y) * BUF_W;
		for (int x = x0; x < x1; ++x)
		{
			int sx = static_cast<int>((x + 0.5f - left) * inv);
			if (sx < 0) sx = 0;
			if (sx >= t.w) sx = t.w - 1;
			const uint32_t px = srcRow[sx];
			const unsigned a = px >> 24;
			if (a == 255) { dst[x] = px; continue; }
			if (a == 0) continue;
			const uint32_t d = dst[x];
			auto mix = [&](unsigned shift) { return ((((px >> shift) & 255) * a + ((d >> shift) & 255) * (255 - a)) / 255) << shift; };
			dst[x] = 0xFF000000u | mix(0) | mix(8) | mix(16);
		}
	}
}

// ----------------------------------------------------------------- GU setup ---

unsigned int __attribute__((aligned(16))) g_list[262144];
unsigned g_drawOffset = 0; // VRAM offset of the buffer being drawn this frame

void guInit()
{
	sceGuInit();
	sceGuStart(GU_DIRECT, g_list);
	sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void *>(0), BUF_W);
	sceGuDispBuffer(SCR_W, SCR_H, reinterpret_cast<void *>(FB_BYTES), BUF_W);
	sceGuDepthBuffer(reinterpret_cast<void *>(FB_BYTES * 2), BUF_W);
	sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
	sceGuViewport(2048, 2048, SCR_W, SCR_H);
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, SCR_W, SCR_H);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuFrontFace(GU_CW);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuShadeModel(GU_SMOOTH);
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	g_drawOffset = 0;
}

struct SpriteVertex { float u, v; float x, y, z; };

void drawTexture(const Texture &t, float cx, float cy, float zoom, bool smooth)
{
	sceGuTexFlush();
	sceGuTexSync();
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, t.tw, t.th, t.tw, t.pixels);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(smooth ? GU_LINEAR : GU_NEAREST, smooth ? GU_LINEAR : GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);

	const float hw = t.w * zoom * 0.5f, hh = t.h * zoom * 0.5f;
	SpriteVertex *v = static_cast<SpriteVertex *>(sceGuGetMemory(2 * sizeof(SpriteVertex)));
	v[0] = { 0.0f, 0.0f, cx - hw, cy - hh, 0.0f };
	v[1] = { static_cast<float>(t.w) / t.tw, static_cast<float>(t.h) / t.th, cx + hw, cy + hh, 0.0f };
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, v);
	sceGuDisable(GU_TEXTURE_2D);
}

struct ColorVertex { unsigned color; float x, y, z; };

// Untextured gradient quad: if this shows but the GPU texture does not, the problem
// is texture upload/sampling, not the GPU or the display setup.
void drawGradientQuad(float x, float y, float size)
{
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_BLEND);
	ColorVertex *v = static_cast<ColorVertex *>(sceGuGetMemory(4 * sizeof(ColorVertex)));
	v[0] = { 0xFF0000FF, x, y, 0.0f };               // red   (ABGR)
	v[1] = { 0xFF00FF00, x + size, y, 0.0f };        // green
	v[2] = { 0xFFFF0000, x, y + size, 0.0f };        // blue
	v[3] = { 0xFFFFFFFF, x + size, y + size, 0.0f }; // white
	sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, nullptr, v);
}

void beginFrame(unsigned clearColor)
{
	sceGuStart(GU_DIRECT, g_list);
	sceGuClearColor(clearColor);
	sceGuClear(GU_COLOR_BUFFER_BIT);
}

// Finishes GPU work, then lets the caller print text into the SAME buffer before the swap.
void endFrameGpu()
{
	sceGuFinish();
	sceGuSync(0, 0);
	pspDebugScreenSetOffset(static_cast<int>(g_drawOffset));
}

// Presents on every 2nd vblank (exactly 30 fps). See psp/PspFramePacer.h.
void presentFrame(PspFramePacer &pacer)
{
	unsigned waits = pacer.waitsNeeded(sceDisplayGetVcount());
	while (waits--)
		sceDisplayWaitVblankStart();
	pacer.presented(sceDisplayGetVcount());
	sceGuSwapBuffers();
	g_drawOffset = g_drawOffset == 0 ? FB_BYTES : 0;
}

// ------------------------------------------------------------ control tally ---

struct Seen
{
	unsigned bits = 0;
};
enum SeenBit : unsigned
{
	S_MOVE = 1u << 0, S_LOOKYAW = 1u << 1, S_LOOKPITCH = 1u << 2, S_BREAK = 1u << 3,
	S_PLACE = 1u << 4, S_JUMP = 1u << 5, S_SNEAK = 1u << 6, S_SPRINT = 1u << 7,
	S_FLY = 1u << 8, S_HOTBAR = 1u << 9, S_INV = 1u << 10, S_PAUSE = 1u << 11,
};
const struct { unsigned bit; const char *name; } kSeenNames[] = {
	{S_MOVE, "move"}, {S_LOOKYAW, "yaw"}, {S_LOOKPITCH, "pitch"}, {S_BREAK, "break"},
	{S_PLACE, "place"}, {S_JUMP, "jump"}, {S_SNEAK, "sneak"}, {S_SPRINT, "sprint"},
	{S_FLY, "fly"}, {S_HOTBAR, "hotbar"}, {S_INV, "inv"}, {S_PAUSE, "pause"},
};

void tally(Seen &seen, const PspControls::Actions &a)
{
	if (a.moveX != 0.f || a.moveZ != 0.f) seen.bits |= S_MOVE;
	if (a.lookYaw != 0.f) seen.bits |= S_LOOKYAW;
	if (a.lookPitch != 0.f) seen.bits |= S_LOOKPITCH;
	if (a.breakBlock) seen.bits |= S_BREAK;
	if (a.placeOrUse) seen.bits |= S_PLACE;
	if (a.jump) seen.bits |= S_JUMP;
	if (a.sneak) seen.bits |= S_SNEAK;
	if (a.sprint) seen.bits |= S_SPRINT;
	if (a.toggleFly) seen.bits |= S_FLY;
	if (a.hotbarDelta != 0) seen.bits |= S_HOTBAR;
	if (a.openInventory) seen.bits |= S_INV;
	if (a.pause) seen.bits |= S_PAUSE;
}

PspControls::RawPad readPad()
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);
	PspControls::RawPad raw;
	raw.buttons = pad.Buttons;
	raw.lx = pad.Lx;
	raw.ly = pad.Ly;
	return raw;
}

// Every button the mapper uses must have the SDK's value, or the layout is silently wrong.
static_assert(static_cast<unsigned>(PspControls::BTN_START) == static_cast<unsigned>(PSP_CTRL_START), "mask");
static_assert(static_cast<unsigned>(PspControls::BTN_CROSS) == static_cast<unsigned>(PSP_CTRL_CROSS), "mask");
static_assert(static_cast<unsigned>(PspControls::BTN_L) == static_cast<unsigned>(PSP_CTRL_LTRIGGER), "mask");
static_assert(static_cast<unsigned>(PspControls::BTN_R) == static_cast<unsigned>(PSP_CTRL_RTRIGGER), "mask");

const char *const kCandidates[] = {
	"assets/terrain.png", "assets/gui/gui.png", "assets/gui/items.png", "assets/font/default.png",
	"assets/particles.png", "assets/environment/clouds.png", "assets/title/mojang.png",
	"assets/title/mclogo.png", "assets/title/bg/panorama0.png", "assets/mob/pig.png",
	"assets/mob/char.png", "assets/pack.png",
};
constexpr int kCandidateCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

float fitZoom(const Texture &t)
{
	float z = std::fmin(236.0f / t.h, 228.0f / t.w);   // two copies fit side by side
	if (z > 1.0f) z = std::floor(z);   // whole-number zoom keeps pixels crisp
	return z;
}

// Logs the RGBA of a few known pixels, so a bad decode shows up in the log as numbers.
void logProbe(const Texture &t)
{
	if (!g_log || !t.pixels) return;
	static const int pts[][2] = {{8, 8}, {24, 8}, {40, 8}, {8, 24}, {72, 40}, {136, 8}, {200, 100}, {248, 248}};
	fprintf(g_log, "PROBE   %s (RGBA, as bytes in memory)\n", t.key);
	for (const auto &pt : pts)
	{
		if (pt[0] >= t.w || pt[1] >= t.h) continue;
		const uint32_t px = t.pixels[static_cast<size_t>(pt[1]) * t.tw + pt[0]];
		fprintf(g_log, "        (%3d,%3d) = %3u %3u %3u %3u\n", pt[0], pt[1], static_cast<unsigned>(px & 255),
		        static_cast<unsigned>((px >> 8) & 255), static_cast<unsigned>((px >> 16) & 255), static_cast<unsigned>(px >> 24));
	}
	fflush(g_log);
}

unsigned g_lastPadLogged = 0xFFFFFFFF;
void logPadChange(unsigned buttons)
{
	if (buttons == g_lastPadLogged) return;
	g_lastPadLogged = buttons;
	if (g_log) { fprintf(g_log, "PAD     %06X  t=%llu\n", buttons, static_cast<unsigned long long>(nowMs())); fflush(g_log); }
}

} // namespace

int main(int argc, char **argv)
{
	pspDebugInstallErrorHandler(crashHandler);
	setupExitCallback();
	pspDebugScreenInit();
	scePowerSetClockFrequency(333, 333, 166);

	char dir[256];
	appDir(argc > 0 ? argv[0] : nullptr, dir, sizeof(dir));
	snprintf(g_logPath, sizeof(g_logPath), "%s/datacheck.log", dir);
	g_log = fopen(g_logPath, "w");
	stage("main: log opened");

	// ------------------------------------------------------------ report ---
	say(COLOR_WHITE, "OptiCraft PSP data check\n------------------------\n");
	say(COLOR_WHITE, "APPDIR  %s\n", dir);
	say(g_log ? COLOR_WHITE : COLOR_RED, "LOG     %s\n", g_log ? "datacheck.log written beside the EBOOT" : "cannot open datacheck.log (read-only?)");

	stage("mount pak");
	const bool mounted = AssetPak::mountFrom(dir);
	say(mounted ? COLOR_GREEN : COLOR_RED, "PAK     %s\n", mounted ? "assets.pak mounted" : "assets.pak NOT FOUND next to the EBOOT");

	int present[kCandidateCount] = {};
	int presentCount = 0;
	if (mounted)
	{
		std::vector<std::string> top;
		AssetPak::listChildren("assets", top);
		say(COLOR_WHITE, "        'assets/' has %u entries\n", static_cast<unsigned>(top.size()));
		for (int i = 0; i < kCandidateCount; ++i)
			if (AssetPak::exists(kCandidates[i])) present[presentCount++] = i;
		say(COLOR_WHITE, "        %d of %d known textures present\n", presentCount, kCandidateCount);
	}

	// First texture: report timing and a checksum so results can be compared across builds.
	Texture tex;
	const char *why = "";
	bool haveTex = false;
	if (presentCount > 0)
	{
		stage("load+decode first texture");
		haveTex = loadFromPak(kCandidates[present[0]], tex, &why);
		if (haveTex)
		{
			say(COLOR_GREEN, "DECODE  %s\n", tex.key);
			say(COLOR_WHITE, "        %dx%d (GPU buffer %dx%d), alpha=%s\n", tex.w, tex.h, tex.tw, tex.th, tex.hasAlpha ? "yes" : "no");
			say(COLOR_WHITE, "        read+decode %llu ms, pixel sum %08lX\n", static_cast<unsigned long long>(tex.loadMs), static_cast<unsigned long>(tex.checksum));
			logProbe(tex);
		}
		else say(COLOR_RED, "DECODE  %s failed: %s\n", kCandidates[present[0]], why);
	}
	if (!haveTex)
	{
		haveTex = makePattern(tex);
		say(COLOR_YELLOW, "        using the built-in test pattern instead\n");
	}

	say(COLOR_WHITE, "MEMORY  %u KB free\n", static_cast<unsigned>(sceKernelTotalFreeMemSize() / 1024));
	say(COLOR_WHITE, "\nPress CROSS to open the viewer (START quits).\n");
	stage("report done, waiting for CROSS");

	// Wait for CROSS (edge) or START.
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	unsigned prev = 0xFFFFFFFF;
	while (g_running)
	{
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
		logPadChange(pad.Buttons);
		if ((pad.Buttons & PSP_CTRL_CROSS) && !(prev & PSP_CTRL_CROSS) && prev != 0xFFFFFFFF) break;
		if (pad.Buttons & PSP_CTRL_START) { g_running = false; break; }
		prev = pad.Buttons;
		sceDisplayWaitVblankStart();
	}

	// ------------------------------------------------------------ viewer ---
	if (g_running && haveTex)
	{
		stage("guInit");
		guInit();
		stage("guInit done");
		pspDebugScreenEnableBackColor(0);

		PspFramePacer pacer(2);      // 30 fps lock: present every 2nd vblank
		PspControls::Config cfg;     // defaults: LookButtons layout, 25% deadzone
		PspControls::State state;
		Seen seen;
		int index = 0;               // position within present[]
		float cx = SCR_W * 0.5f, cy = SCR_H * 0.5f, zoom = fitZoom(tex);
		bool smooth = false;
		uint64_t startHeldSince = 0;
		uint64_t last = nowMs(), fpsT = last;
		int frames = 0, fps = 0;
		unsigned totalFrames = 0;
		const uint64_t started = last;
		int switchFailures = 0;

		while (g_running)
		{
			const uint64_t now = nowMs();
			float dt = (now - last) / 1000.0f;
			last = now;
			if (dt > 0.1f) dt = 0.1f;

			const PspControls::RawPad rawPad = readPad();
			logPadChange(rawPad.buttons);
			const PspControls::Actions a = PspControls::map(rawPad, cfg, state, static_cast<unsigned>(now));
			tally(seen, a);
			// Quit only by HOLDING Start, so it cannot be hit by accident.
			if (rawPad.buttons & PspControls::BTN_START)
			{
				if (startHeldSince == 0) startHeldSince = now;
				if (now - startHeldSince >= 1500) break;
			}
			else startHeldSince = 0;

			cx += a.moveX * 220.0f * dt;
			cy -= a.moveZ * 220.0f * dt;
			zoom *= std::exp(a.lookPitch * 1.3f * dt);
			if (zoom < 0.25f) zoom = 0.25f;
			if (zoom > 16.0f) zoom = 16.0f;
			if (a.jump) { cx = SCR_W * 0.5f; cy = SCR_H * 0.5f; zoom = fitZoom(tex); }
			if (a.openInventory) smooth = !smooth;

			if (a.hotbarDelta != 0 && presentCount > 1)
			{
				index = (index + a.hotbarDelta + presentCount) % presentCount;
				Texture next;
				if (loadFromPak(kCandidates[present[index]], next, &why))
				{
					freeTexture(tex);
					tex = next;
					cx = SCR_W * 0.5f; cy = SCR_H * 0.5f; zoom = fitZoom(tex);
					if (g_log) fprintf(g_log, "VIEW    %s %dx%d %llu ms sum %08lX\n", tex.key, tex.w, tex.h,
					                   static_cast<unsigned long long>(tex.loadMs), static_cast<unsigned long>(tex.checksum));
				}
				else
				{
					++switchFailures;
					if (g_log) fprintf(g_log, "VIEWFAIL %s: %s\n", kCandidates[present[index]], why);
				}
			}

			const float leftCx = cx - 122.0f, rightCx = cx + 122.0f;
			if (totalFrames < 3) { char m[48]; snprintf(m, sizeof(m), "frame %u draw", totalFrames + 1); stage(m); }
			beginFrame(0xFF303030);
			drawGradientQuad(cx - 6.0f, cy - 6.0f, 12.0f);   // GPU sanity: untextured gradient
			drawTexture(tex, rightCx, cy, zoom, smooth);     // GPU path (right)
			endFrameGpu();
			cpuBlit(tex, g_drawOffset, leftCx, cy, zoom);    // CPU path (left), after the GPU is done

			pspDebugScreenSetXY(0, 0);
			pspDebugScreenSetTextColor(COLOR_WHITE);
			pspDebugScreenPrintf("%s  %dx%d  zoom %.2f  %s\n", tex.key, tex.w, tex.h, zoom, smooth ? "smooth" : "sharp");
			pspDebugScreenSetTextColor(pacer.slowFrames() == 0 ? COLOR_GREEN : COLOR_RED);
			pspDebugScreenPrintf("%d fps  30fps lock: %.1f%% on time, %u slow, worst %u vblanks\n", fps,
			                     pacer.lockedRatio() * 100.0f, pacer.slowFrames(), pacer.worstIntervalVblanks());
			pspDebugScreenSetXY(2, 3);
			pspDebugScreenSetTextColor(COLOR_WHITE);
			pspDebugScreenPrintf("CPU (left)");
			pspDebugScreenSetXY(33, 3);
			pspDebugScreenPrintf("GPU (right)");
			pspDebugScreenSetXY(0, 2);
			pspDebugScreenSetTextColor(COLOR_YELLOW);
			pspDebugScreenPrintf("MOVE %+.2f %+.2f  LOOK %+.0f %+.0f  brk%d use%d jmp%d snk%d spr%d fly%d\n",
			                     a.moveX, a.moveZ, a.lookYaw, a.lookPitch, a.breakBlock, a.placeOrUse,
			                     a.jump, a.sneak, a.sprint, state.flying);
			pspDebugScreenSetXY(0, 32);
			pspDebugScreenSetTextColor(0xFFC0C0C0);
			pspDebugScreenPrintf("HOLD START 1.5s to quit | Select: sharp/smooth | D-pad L/R: next\n");
			// Controls tally along the bottom: lit = seen at least once.
			pspDebugScreenSetXY(0, 33);
			pspDebugScreenSetTextColor(COLOR_WHITE);
			pspDebugScreenPrintf("Tested: ");
			for (const auto &s : kSeenNames)
			{
				pspDebugScreenSetTextColor((seen.bits & s.bit) ? COLOR_GREEN : 0xFF808080);
				pspDebugScreenPrintf("%s ", s.name);
			}
			presentFrame(pacer);
			if (totalFrames < 3) { char m[48]; snprintf(m, sizeof(m), "frame %u presented", totalFrames + 1); stage(m); }
			if (totalFrames == 3) stage("viewer running");

			++frames; ++totalFrames;
			if (now - fpsT >= 1000) { fps = frames; frames = 0; fpsT = now; }
		}

		if (g_log)
		{
			const uint64_t ms = nowMs() - started + 1;
			fprintf(g_log, "\nVIEWER  %u frames in %llu ms (%.1f fps avg), switch failures %d\n", totalFrames,
			        static_cast<unsigned long long>(ms), totalFrames * 1000.0 / ms, switchFailures);
			fprintf(g_log, "LOCK30  %.2f%% of frames on time, %u slow, worst interval %u vblanks (2 = perfect)\n",
			        pacer.lockedRatio() * 100.0f, pacer.slowFrames(), pacer.worstIntervalVblanks());
			fprintf(g_log, "TESTED  ");
			for (const auto &s : kSeenNames) fprintf(g_log, "%s=%s ", s.name, (seen.bits & s.bit) ? "yes" : "no");
			fprintf(g_log, "\n");
			fflush(g_log);
		}
		stage("exit: sceGuTerm");
		sceGuTerm();
		stage("exit: after sceGuTerm");
	}

	stage("exit: free + close");
	freeTexture(tex);
	if (g_log) fclose(g_log);
	sceKernelExitGame();
	return 0;
}
