#pragma once

// PspTuning.h — PSP-2000+ (64 MB, 333 MHz Allegrex) tuning decisions.
//
// Not wired into platform/PlatformTuning.h yet: that happens together with the
// first full PSP backend (see PSP_PORT.md). Values here come from measurements
// on a real console (bring-up log), not guesses.

// Visible terrain radius in chunks. 3 = a 7x7 chunk area around the player
// (112x112 blocks), a step up from the Wii's 2 while still fitting the PSP's
// 480x272 screen and ~50 MB heap. Revisit after the first on-device FPS numbers.
#define PSP_VISIBLE_CHUNK_RADIUS        3
#define PSP_VISIBLE_CHUNK_DIAMETER      (PSP_VISIBLE_CHUNK_RADIUS * 2 + 1)

// Measured on hardware: 50944 KB obtainable via malloc with MEMSIZE=1.
// Keep headroom for stack, GE command lists and the framebuffers.
#define PSP_HEAP_BUDGET_KB              48000
#define PSP_HEAP_RESERVE_KB             4096

// Measured: `double` math is ~57x slower than `float` (software emulation; the
// Allegrex FPU is single precision only). Hot paths (entities, physics, chunk
// math, noise) must not touch double on this target.
#define PSP_FLOAT_ONLY_HOT_PATHS        1

// Memory Stick throughput measured: write 4.7 MB/s, read 1.3 MB/s. Region/chunk
// IO must be batched, asynchronous and never on the render thread.
#define PSP_MS_READ_MBPS_X10            13
#define PSP_MS_WRITE_MBPS_X10           47

// Frame rate policy: PSP-2000 and later only, locked to 30 fps (every 2nd vblank).
// The lock caps the rate; HOLDING 30 under load is the job of the render-distance
// governor (3 chunks by default, stepping down to 2 after sustained slow frames
// and back up when there is headroom), driven by PspFramePacer::slowFramesInARow().
#define PSP_TARGET_FPS                  30
#define PSP_FRAME_VBLANKS               2
#define PSP_MIN_CHUNK_RADIUS            2
#define PSP_SLOW_FRAMES_TO_STEP_DOWN    6   // ~0.2 s of consecutive slow frames
