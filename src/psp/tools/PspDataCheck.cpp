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

#include <pspkernel.h>
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
	float z = std::fmin(236.0f / t.h, 440.0f / t.w);
	if (z > 1.0f) z = std::floor(z);   // whole-number zoom keeps pixels crisp
	return z;
}

} // namespace

int main(int argc, char **argv)
{
	setupExitCallback();
	pspDebugScreenInit();
	scePowerSetClockFrequency(333, 333, 166);

	char dir[256], logPath[300];
	appDir(argc > 0 ? argv[0] : nullptr, dir, sizeof(dir));
	snprintf(logPath, sizeof(logPath), "%s/datacheck.log", dir);
	g_log = fopen(logPath, "w");

	// ------------------------------------------------------------ report ---
	say(COLOR_WHITE, "OptiCraft PSP data check\n------------------------\n");
	say(COLOR_WHITE, "APPDIR  %s\n", dir);
	say(g_log ? COLOR_WHITE : COLOR_RED, "LOG     %s\n", g_log ? "datacheck.log written beside the EBOOT" : "cannot open datacheck.log (read-only?)");

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
		haveTex = loadFromPak(kCandidates[present[0]], tex, &why);
		if (haveTex)
		{
			say(COLOR_GREEN, "DECODE  %s\n", tex.key);
			say(COLOR_WHITE, "        %dx%d (GPU buffer %dx%d), alpha=%s\n", tex.w, tex.h, tex.tw, tex.th, tex.hasAlpha ? "yes" : "no");
			say(COLOR_WHITE, "        read+decode %llu ms, pixel sum %08lX\n", static_cast<unsigned long long>(tex.loadMs), static_cast<unsigned long>(tex.checksum));
		}
		else say(COLOR_RED, "DECODE  %s failed: %s\n", kCandidates[present[0]], why);
	}
	if (!haveTex)
	{
		haveTex = makePattern(tex);
		say(COLOR_YELLOW, "        using the built-in test pattern instead\n");
	}

	say(COLOR_WHITE, "MEMORY  %u KB free\n", static_cast<unsigned>(sceKernelTotalFreeMemSize() / 1024));
	say(COLOR_WHITE, "\nPress CROSS to open the viewer, START to quit.\n");

	// Wait for CROSS (edge) or START.
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	unsigned prev = 0xFFFFFFFF;
	while (g_running)
	{
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
		if ((pad.Buttons & PSP_CTRL_CROSS) && !(prev & PSP_CTRL_CROSS) && prev != 0xFFFFFFFF) break;
		if (pad.Buttons & PSP_CTRL_START) { g_running = false; break; }
		prev = pad.Buttons;
		sceDisplayWaitVblankStart();
	}

	// ------------------------------------------------------------ viewer ---
	if (g_running && haveTex)
	{
		guInit();
		pspDebugScreenEnableBackColor(0);

		PspFramePacer pacer(2);      // 30 fps lock: present every 2nd vblank
		PspControls::Config cfg;     // defaults: LookButtons layout, 25% deadzone
		PspControls::State state;
		Seen seen;
		int index = 0;               // position within present[]
		float cx = SCR_W * 0.5f, cy = SCR_H * 0.5f, zoom = fitZoom(tex);
		bool smooth = false;
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

			const PspControls::Actions a = PspControls::map(readPad(), cfg, state, static_cast<unsigned>(now));
			tally(seen, a);
			if (a.pause) break;

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

			beginFrame(0xFF303030);
			drawTexture(tex, cx, cy, zoom, smooth);
			endFrameGpu();

			pspDebugScreenSetXY(0, 0);
			pspDebugScreenSetTextColor(COLOR_WHITE);
			pspDebugScreenPrintf("%s  %dx%d  zoom %.2f  %s\n", tex.key, tex.w, tex.h, zoom, smooth ? "smooth" : "sharp");
			pspDebugScreenSetTextColor(pacer.slowFrames() == 0 ? COLOR_GREEN : COLOR_RED);
			pspDebugScreenPrintf("%d fps  30fps lock: %.1f%% on time, %u slow, worst %u vblanks\n", fps,
			                     pacer.lockedRatio() * 100.0f, pacer.slowFrames(), pacer.worstIntervalVblanks());
			pspDebugScreenSetTextColor(COLOR_YELLOW);
			pspDebugScreenPrintf("MOVE %+.2f %+.2f  LOOK %+.0f %+.0f  brk%d use%d jmp%d snk%d spr%d fly%d\n",
			                     a.moveX, a.moveZ, a.lookYaw, a.lookPitch, a.breakBlock, a.placeOrUse,
			                     a.jump, a.sneak, a.sprint, state.flying);
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
		sceGuTerm();
	}

	freeTexture(tex);
	if (g_log) fclose(g_log);
	sceKernelExitGame();
	return 0;
}
