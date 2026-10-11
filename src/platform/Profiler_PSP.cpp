#include "platform/Profiler.h"

#include <cstdio>
#include <cstring>

#include "psp/system/PspSystem.h"

// PSP profiler: sums the game's tick/streaming phase timings and prints them in boot.log together
// with the perf line (once per 120 frames), then resets. Cheap: a few additions per phase call.
namespace
{
struct Slot { const char *name; long long ns; unsigned calls; };
Slot g_slots[24];
int g_count = 0;

void note(const char *name, long long ns)
{
    for (int i = 0; i < g_count; ++i)
    {
        if (g_slots[i].name == name || std::strcmp(g_slots[i].name, name) == 0)
        {
            g_slots[i].ns += ns;
            ++g_slots[i].calls;
            return;
        }
    }
    if (g_count < 24)
        g_slots[g_count++] = {name, ns, 1};
}
}

namespace PspProfile
{
void dump()
{
    char line[520];
    int n = std::snprintf(line, sizeof(line), "phases(ms/window):");
    for (int i = 0; i < g_count && n < (int)sizeof(line) - 40; ++i)
    {
        if (g_slots[i].ns < 1000000) // < 0.2 ms: not interesting
            continue;
        n += std::snprintf(line + n, sizeof(line) - n, " %s=%lld(%u)", g_slots[i].name, g_slots[i].ns / 1000000, g_slots[i].calls);
    }
    PspSystem::bootTrace("%s\n", line);
    for (int i = 0; i < g_count; ++i) { g_slots[i].ns = 0; g_slots[i].calls = 0; }
}
}

std::uint32_t platformProfileRenderPhaseBegin() { return 0; }
void platformProfileRenderPhaseEnd(std::uint32_t, PlatformRenderPhase) {}
void platformProfileTickPhase(const char *name, long long ns) { note(name, ns); }
void platformProfileChunkBuild(long long, int) {}
void platformProfileChunkMeshPass(int, long long, int) {}
void platformProfileSnowColumn(bool, bool, int) {}
void platformProfilePopulatePhase(PlatformPopulatePhase, long long) {}
void platformProfileChunkLoad(long long ns) { note("chunkLoad", ns); }
void platformProfilePopulate(long long ns) { note("populate", ns); }
void platformProfileGenerate(long long ns) { note("generate", ns); }
void platformProfileMesh(long long ns) { note("mesh", ns); }
void platformProfileUnloadSave(long long ns) { note("unloadSave", ns); }
void platformProfileTickUpdates(long long ns) { note("tickUpdates", ns); }
void platformProfileTickQueue(long long) {}
void platformProfileMobSpawn(long long ns) { note("mobSpawnS", ns); }
void platformProfileSaveWorldInfo(long long ns) { note("saveInfo", ns); }
void platformProfileMapStorage(long long ns) { note("mapStorage", ns); }
void platformProfileChunkEvict(long long ns) { note("chunkEvict", ns); }
