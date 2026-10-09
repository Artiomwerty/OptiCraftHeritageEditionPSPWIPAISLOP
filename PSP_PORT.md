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

## Scope measured (syntax check with psp-g++, all common sources)

- 1092 of 1178 shared source files compile for the PSP as-is.
- The 86 that do not are foreign-platform files (PC/SDL/GL, Wii, PS2) that source
  selection excludes anyway, plus 14 mod/skin files that only need the include dirs
  the PS2 build also adds (`src/net/minecraft/src`, `src/mods`). No real code problems.
- So the remaining work is the PSP layers only:
  - Small facades (12-140 lines each in the PS2 versions): storage, resources,
    diagnostics, settings, profiler, platform policy, screenshots, text/gamepad input.
  - Renderer: implement `platform/RenderAPI.h` (about 100 functions) and
    `platform/RenderTerrainAPI.h` (about 50) on sceGu. The desktop GL path cannot be
    reused: it needs display lists and occlusion queries, which the PSP's GL lacks.
  - Sound: `platform/audio/SoundManager` on sceAudio.
  - Entry point, File/Runtime shims, crash handler (modeled on `src/wii/`).

## Blockers / constraints

- **Game data is not in the repo.** `assets/` holds only a cursor image. A data pack
  (textures, sounds, language files) is needed before anything can be drawn.
  Wii/PS2 builds stage it from `data/assets` and `data/resources`.
- No emulator can be run in the build sandbox, so every visual test happens on the
  real PSP. Work is therefore split into small on-device milestones that each print
  a report to screen and `*.log`.

## Milestones (each ends with an EBOOT to test on device)

1. Storage + data pack: mount assets, read a texture, print results.  
2. sceGu: draw that texture, then the chunk mesh path with fog and culling.  
3. Engine init to main menu (input via `PspControls`).  
4. Walk around a 3-chunk world; tune FPS; float-only hot paths.  
5. Sound, saves, settings.

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

Settings: nub deadzone (default 25%), look speed, jump double-tap window.

The Pencea-Flavius/Minecraft-PE-PSP layout could not be retrieved (its README only
documents a DualShock layout and that repository is GPL-3.0, so it is reference only,
never copied). This layout is a standard handheld scheme chosen instead.
