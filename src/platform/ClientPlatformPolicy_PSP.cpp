#include <pspiofilemgr.h>
#include <string>
#include "psp/system/PspSystem.h"
#include "platform/ClientPlatformPolicy.h"

#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/GameSettings.h"
#include "pc/CrashHandler.h"
#include "platform/PlatformConfig.h"
#include "platform/GameSettingsBackend.h"

namespace ClientPlatformPolicy
{
int initialWidth()
{
    return 480;
}

int initialHeight()
{
    return 272;
}

std::string minecraftDirectory()
{
    // Worlds and options live in the save folder of the same storage the game runs from
    // (ms0: on a Memory Stick, ef0: on the PSP Go's internal storage), not next to the EBOOT.
    std::string dev = "ms0:";
    const std::string app = PspSystem::appDir();
    const std::size_t colon = app.find(':');
    if (colon != std::string::npos && colon <= 4)
        dev = app.substr(0, colon + 1);
    sceIoMkdir((dev + "/PSP").c_str(), 0777);
    sceIoMkdir((dev + "/PSP/SAVEDATA").c_str(), 0777);
    sceIoMkdir((dev + "/PSP/SAVEDATA/OPTICRAFT").c_str(), 0777);
    return dev + "/PSP/SAVEDATA/OPTICRAFT";
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings* settings)
{
#if PLATFORM_PC_LEGACY
    if (settings == nullptr)
        return;

    settings->particleSetting = 2;
    settings->fancyGraphics = false;
    settings->ambientOcclusion = false;
    settings->ofVoidParticles = false;
    settings->ofWaterParticles = false;
    settings->ofRainSplash = false;
    settings->ofPortalParticles = false;
    settings->ofDrippingWaterLava = false;
    settings->ofWeather = false;
    settings->legacyCrafting = true;
    settings->ofRenderDistanceFine = 40; // ~2.5 chunks
    platformGameSettingsUpdateRenderDistanceFromFine(settings->ofRenderDistanceFine, settings->renderDistance);
    platformGameSettingsApplyLegacyCrafting(*settings);
#ifdef PSP_NO_SKY
    settings->ofSky = false;
    settings->ofSunMoon = false;
#else
    settings->ofSky = true;
    settings->ofSunMoon = true;
#endif
    settings->ofClouds = 1; // fast clouds
#else
    (void)settings;
#endif
}

void preloadStartupTextures(RenderEngine*)
{
}

void releaseWorldEntryAssets(RenderEngine*)
{
}

int panoramaSampleGrid()
{
    // One pass: the 8x8 accumulation was 384 draws per frame (about 1.6 s per frame on a real PSP).
    return 1;
}


void reportCrash(const std::string& description)
{
    CrashHandler::Crash(description);
}
}
