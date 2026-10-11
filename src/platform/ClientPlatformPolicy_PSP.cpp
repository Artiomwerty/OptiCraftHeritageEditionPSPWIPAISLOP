#include <pspiofilemgr.h>
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
    // Worlds and options live in the Memory Stick's save folder, not next to the EBOOT.
    sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
    sceIoMkdir("ms0:/PSP/SAVEDATA/OPTICRAFT", 0777);
    return "ms0:/PSP/SAVEDATA/OPTICRAFT";
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
    return 8;
}


void reportCrash(const std::string& description)
{
    CrashHandler::Crash(description);
}
}
