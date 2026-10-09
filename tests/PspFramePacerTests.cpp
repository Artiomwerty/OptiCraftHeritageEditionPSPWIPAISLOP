// Host-side tests for the PSP 30 fps frame pacer (no PSP SDK needed).
#include "psp/PspFramePacer.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// Simulates a loop where each frame's work takes `work` vblanks. The display is
// 1 vblank per tick; waits advance the counter to the requested vblank.
static unsigned runFrames(PspFramePacer &p, unsigned &vc, unsigned frames, unsigned work)
{
	unsigned presents = 0;
	for (unsigned i = 0; i < frames; ++i)
	{
		vc += work;                          // frame work
		unsigned n = p.waitsNeeded(vc);
		vc += n;                             // each wait blocks until the next vblank
		p.presented(vc);
		++presents;
	}
	return presents;
}

int main()
{
	// Fast frames (work finishes within the same vblank) lock to exactly 2 vblanks.
	{
		PspFramePacer p; unsigned vc = 100;
		runFrames(p, vc, 120, 0);
		CHECK(p.slowFrames() == 0);
		CHECK(p.lockedRatio() == 1.0f);
		// 120 frames, 119 measured intervals of 2 vblanks (the 1st has no baseline).
		CHECK(vc - 100 >= 2 * 119);
		CHECK(vc - 100 <= 2 * 120 + 2);
	}
	// Work that takes 1 vblank still presents on the 2-vblank boundary.
	{
		PspFramePacer p; unsigned vc = 5;
		runFrames(p, vc, 60, 1);
		CHECK(p.slowFrames() == 0);
	}
	// Work that takes 2 vblanks leaves no slack: waits 1 to reach the next vblank,
	// so the interval is 3 and every such frame counts as slow (20 fps territory).
	{
		PspFramePacer p; unsigned vc = 0;
		runFrames(p, vc, 30, 2);
		CHECK(p.slowFrames() > 0);
		CHECK(p.worstIntervalVblanks() >= 3);
	}
	// A slow frame is presented at the very next vblank (not delayed further).
	{
		PspFramePacer p; unsigned vc = 10;
		p.presented(vc);                    // baseline
		vc += 5;                            // a heavy frame: 5 vblanks of work
		CHECK(p.waitsNeeded(vc) == 1);
	}
	// Consecutive-slow counter resets after a good frame.
	{
		PspFramePacer p; unsigned vc = 0;
		runFrames(p, vc, 5, 0);
		runFrames(p, vc, 3, 4);
		CHECK(p.slowFramesInARow() >= 1);
		runFrames(p, vc, 3, 0);
		CHECK(p.slowFramesInARow() == 0);
	}
	// The vblank counter wraps at 2^32; pacing must survive it.
	{
		PspFramePacer p; unsigned vc = 0xFFFFFFF0u;
		runFrames(p, vc, 40, 0);            // crosses the wrap
		CHECK(p.slowFrames() == 0);
	}
	// A 1-vblank budget (60 fps) is supported too.
	{
		PspFramePacer p(1); unsigned vc = 0;
		runFrames(p, vc, 30, 0);
		CHECK(p.slowFrames() == 0);
		CHECK(vc >= 30 && vc <= 32);
	}

	if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
	std::printf("PspFramePacerTests: all passed\n");
	return 0;
}
