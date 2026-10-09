// PspBringup.cpp — PSP toolchain/hardware smoke test for OptiCraft.
//
// NOT the game entry point. It proves the pspdev toolchain, EBOOT packaging and
// the console agree, and reports the numbers the port is sized against:
// CPU clock, free RAM (is MEMSIZE=1 granting the PSP-2000+ memory?), type sizes,
// float vs double speed (the PSP FPU is single precision only), and Memory Stick
// throughput. The same report is written to bringup.log beside the EBOOT.
//
// Built only when PSP_BRINGUP=ON.

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

PSP_MODULE_INFO("OptiCraftBringup", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// Leave a little RAM for the kernel/stack; the rest is the malloc heap.
PSP_HEAP_SIZE_KB(-1024);

namespace
{

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

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...)
{
	char buf[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
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

uint64_t nowMs()
{
	return sceKernelGetSystemTimeWide() / 1000ull;
}

// Same loop in float and double. The Allegrex FPU has no double hardware, so the
// double version runs in software; this prints how bad it is on this console.
volatile float  g_fSink;
volatile double g_dSink;

uint64_t benchFloat()
{
	const uint64_t t0 = nowMs();
	float a = 1.0f, b = 1.0001f;
	for (int i = 0; i < 2000000; ++i) { a = a * b + 0.0001f; if (a > 1000.0f) a = 1.0f; }
	g_fSink = a;
	return nowMs() - t0;
}

uint64_t benchDouble()
{
	const uint64_t t0 = nowMs();
	double a = 1.0, b = 1.0001;
	for (int i = 0; i < 2000000; ++i) { a = a * b + 0.0001; if (a > 1000.0) a = 1.0; }
	g_dSink = a;
	return nowMs() - t0;
}

void storageTest(const char *dir)
{
	const size_t size = 1u << 20;               // 1 MB
	const int blocks = 5;
	unsigned char *buf = static_cast<unsigned char *>(malloc(size));
	if (!buf) { say("STORAGE no memory for the write test\n"); return; }
	for (size_t i = 0; i < size; ++i) buf[i] = static_cast<unsigned char>(i * 31u + 7u);

	char path[300];
	snprintf(path, sizeof(path), "%s/writetest.bin", dir);
	FILE *f = fopen(path, "wb");
	if (!f) { say("STORAGE cannot write to %s\n", path); free(buf); return; }
	uint64_t t0 = nowMs();
	for (int i = 0; i < blocks; ++i) fwrite(buf, 1, size, f);
	fclose(f);
	const uint64_t wMs = nowMs() - t0 + 1;

	unsigned char *rd = static_cast<unsigned char *>(malloc(size));
	bool ok = rd != nullptr;
	t0 = nowMs();
	f = fopen(path, "rb");
	if (f && rd)
	{
		for (int i = 0; i < blocks; ++i)
		{
			if (fread(rd, 1, size, f) != size || memcmp(rd, buf, size) != 0) ok = false;
		}
		fclose(f);
	}
	else ok = false;
	const uint64_t rMs = nowMs() - t0 + 1;

	say("STORAGE write %.1f MB/s, read %.1f MB/s, data %s\n",
	    blocks * 1000.0 / wMs, blocks * 1000.0 / rMs, ok ? "verified OK" : "MISMATCH");
	remove(path);
	free(rd);
	free(buf);
}

} // namespace

int main(int argc, char **argv)
{
	setupExitCallback();
	pspDebugScreenInit();
	scePowerSetClockFrequency(333, 333, 166);

	char dir[256], logPath[300];
	appDir(argc > 0 ? argv[0] : nullptr, dir, sizeof(dir));
	snprintf(logPath, sizeof(logPath), "%s/bringup.log", dir);
	g_log = fopen(logPath, "w");

	say("OptiCraft PSP bring-up\n----------------------\n");
	say("APPDIR  %s\n", dir);
	if (g_log) say("LOG     bringup.log written beside the EBOOT\n");
	else       say("LOG     could not open bringup.log (read-only?)\n");

	say("CLOCK   cpu %d MHz  bus %d MHz\n", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());

	const uint16_t probe = 1;
	say("TYPES   %s-endian  long=%u ptr=%u double=%u bytes\n",
	    *reinterpret_cast<const uint8_t *>(&probe) == 1 ? "little" : "big",
	    (unsigned)sizeof(long), (unsigned)sizeof(void *), (unsigned)sizeof(double));

	say("KERNEL  %u KB free, largest block %u KB\n",
	    (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024));

	// Probe how much malloc can really hand out, in 256 KB steps.
	unsigned heapKb = 0;
	void *chain = nullptr;
	for (;;)
	{
		void **blk = static_cast<void **>(malloc(256 * 1024));
		if (!blk) break;
		*blk = chain; chain = blk; heapKb += 256;
	}
	while (chain) { void *next = *static_cast<void **>(chain); free(chain); chain = next; }
	say("HEAP    %u KB obtainable via malloc\n", heapKb);
	if (heapKb > 40u * 1024u) say("        -> 64 MB mode is ON (PSP-2000+, MEMSIZE=1 works)\n");
	else                      say("        -> only 32 MB usable (PSP-1000, or extra RAM not granted)\n");

	say("FLOAT   running benchmark...\n");
	const uint64_t fMs = benchFloat() + 1, dMs = benchDouble() + 1;
	say("        float  %llu ms\n", (unsigned long long)fMs);
	say("        double %llu ms\n", (unsigned long long)dMs);
	say("        double is ~%llux slower\n", (unsigned long long)(dMs / fMs));

	storageTest(dir);

	say("----------------------\nDone. Press START to quit.\n");
	if (g_log) { fclose(g_log); g_log = nullptr; }

	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	while (g_running)
	{
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_START) break;
		sceDisplayWaitVblankStart();
	}
	sceKernelExitGame();
	return 0;
}
