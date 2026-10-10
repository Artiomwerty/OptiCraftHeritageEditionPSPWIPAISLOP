// PSP sound manager.
//
// Silent for now: music and effects are deferred (the first PSP milestones are about getting
// the game itself running). Every SoundManager entry point exists and does nothing, so the
// rest of the engine runs unchanged. The real sceAudio mixer replaces this file.
#include "net/minecraft/src/SoundManager.h"

void *SoundManager::sndSystem = nullptr;
bool SoundManager::loaded = false;

SoundManager::SoundManager() : soundVolume(0), options(nullptr), ticksBeforeMusic(0) {}
void SoundManager::loadSoundSettings(GameSettings *gamesettings) { options = gamesettings; }
void SoundManager::onSoundOptionsChanged() {}
void SoundManager::closeMinecraft() {}
void SoundManager::addSound(const jstring &, const std::string &) {}
void SoundManager::addStreaming(const jstring &, const std::string &) {}
void SoundManager::addMusic(const jstring &, const std::string &) {}
void SoundManager::playRandomMusicIfReady() {}
bool SoundManager::playMusicFileNow(const std::string &) { return false; }
void SoundManager::setListenerPosition(EntityLiving *, float) {}
void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}
void SoundManager::playSound(const jstring &, float, float, float, float, float) {}
void SoundManager::playSoundFX(const jstring &, float, float) {}
void SoundManager::tryToSetLibraryAndCodecs() {}
