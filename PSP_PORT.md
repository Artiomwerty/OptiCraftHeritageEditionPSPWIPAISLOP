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

## Controls (decided)

Implemented in `src/psp/input/PspControls.{h,cpp}` as a hardware-free mapper with host
tests (`tests/PspControlsTests.cpp`). The sceCtrl poller will feed it.

Default layout ("LookButtons"; the PSP has one analog nub):

| Input | Action |
|---|---|
| Analog nub | Move |
| Triangle / Cross / Square / Circle | Look up / down / left / right |
| R (hold) | Break block |
| L | Place / use |
| L + R | Toggle sprint |
| D-pad left / right | Previous / next hotbar slot |
| D-pad up | Jump (double-tap: toggle fly) |
| D-pad down | Toggle sneak |
| Select | Inventory / crafting |
| Start | Pause |

Alternative ("LookNub", switchable in settings): D-pad moves, nub looks, Cross jumps,
Circle sneaks, Triangle inventory, Square next slot, Select previous slot.

Settings: nub deadzone (default 20%), look speed, jump double-tap window.

The Pencea-Flavius/Minecraft-PE-PSP layout could not be retrieved (its README only
documents a DualShock layout and that repository is GPL-3.0, so it is reference only,
never copied). This layout is a standard handheld scheme chosen instead.
