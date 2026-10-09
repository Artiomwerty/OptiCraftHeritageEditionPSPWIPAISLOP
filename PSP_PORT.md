# PSP port — status and plan

Target: PSP-2000 and later (64 MB RAM, 333 MHz). Build: pspdev (`psp-gcc`).

## Status

| Milestone | State |
|---|---|
| Toolchain + `psp-bringup` preset, EBOOT, zip | done, runs on a real PSP Go |
| Hardware numbers (RAM, float/double, Memory Stick) | measured, see below |
| PSP backend files (`*_PSP.cpp`) | not started |
| sceGu renderer | not started |
| Input (sceCtrl) | not started — needs the control layout, see below |
| Full game boots | not started |

`cmake --preset psp-bringup && cmake --build --preset psp-bringup`
produces `bin/psp/OptiCraft-PSP.zip` (extract at the Memory Stick root).

## Measured on real hardware (PSP Go, ARK-style CFW)

- Heap with `MEMSIZE=1`: 50944 KB obtainable. 64 MB mode works.
- `double` is ~57x slower than `float` (1042 ms vs 18 ms). Hot paths must use float.
- Memory Stick: write 4.7 MB/s, read 1.3 MB/s. Chunk IO must be async and batched.

## Decisions

- Visible chunk radius: 3 (`src/psp/PspTuning.h`).
- Float-only hot paths (`-fsingle-precision-constant` plus a `real_t` pass).

## Backend files to write

Selected per-platform in `cmake/SourceSelection.cmake` (add `PSP` to each regex).
Model each on the Wii or PS2 file of the same name in `src/platform/`:
RenderAPI, RenderTerrainAPI, SoundManager, StorageBackend, InputBackend,
LegacyControlPromptBackend, Resources, Diagnostics, GameSettingsBackend,
PlatformUserSettings, Profiler, ClientProfilerBackend, ClientPlatformPolicy,
ScreenshotBackend, RenderLightingProfile, TextureResidencyPolicy.

## Open question: control layout

Requested layout: Pencea-Flavius/Minecraft-PE-PSP ("Layout 1" is its PSP default).
Its README documents only the DualShock "Layout 4"; Layout 1 is not written down
publicly and that repository is GPL-3.0, so its code is used as reference only,
never copied. The mapping has to be taken from the in-game options screen.
