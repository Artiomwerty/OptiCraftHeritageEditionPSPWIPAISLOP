// PSP sound manager: a small software mixer on top of sceAudio.
//
//  * Effects are PS2-style ADPCM (.adp, APCM header + 16-byte PS-ADPCM blocks). They are decoded
//    once to 16-bit mono PCM (the decoder is the portable one the PS2 port uses), cached, and
//    mixed by up to kVoices voices.
//  * Music is raw 22.05 kHz stereo 16-bit PCM (.pcm) streamed from assets.pak by a low-priority
//    reader thread into a ring buffer. Reads only, so the Memory Stick is not worn by playback.
//  * One mixer thread feeds a 22.05 kHz stereo SRC channel with blocking output; there is no
//    std::thread / std::mutex here (the pthread layer on this platform is unreliable).
#ifdef PSP_NO_AUDIO
// Diagnostic build: completely silent, no statics, no decoder.
#include "net/minecraft/src/SoundManager.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/GameSettings.h"
void *SoundManager::sndSystem = nullptr;
bool SoundManager::loaded = false;
SoundManager::SoundManager() : soundVolume(0), options(nullptr), ticksBeforeMusic(0) {}
void SoundManager::loadSoundSettings(GameSettings *g) { options = g; }
void SoundManager::tryToSetLibraryAndCodecs() {}
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
#else
#include "net/minecraft/src/SoundManager.h"

#include <pspaudio.h>
#include <pspkernel.h>

#include <algorithm>
#include <malloc.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/SoundPoolEntry.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/audio/AudioSpatialization.h"
#include "platform/storage/AssetPak.h"
#include "ps2/audio/Ps2AdpcmStreamDecoder.h"
#include "psp/system/PspSystem.h"

void *SoundManager::sndSystem = nullptr;
bool SoundManager::loaded = false;

namespace
{
constexpr int kOutRate = 22050;
constexpr int kBlockFrames = 512; // multiple of 64
constexpr int kVoices = 8;
constexpr std::size_t kMaxCacheBytes = 3 * 1024 * 1024;
constexpr std::uint32_t kRingBytes = 128 * 1024;
constexpr std::uint32_t kReadChunk = 16 * 1024;

AudioListenerState *g_listenerPtr = nullptr;
#define g_listener (*g_listenerPtr)
float g_soundVolume = 1.0f;
float g_musicVolume = 1.0f;
bool g_started = false;

// ------------------------------------------------------------------------------- samples ---

struct Sample
{
    std::vector<std::int16_t> pcm; // mono
    int rate = kOutRate;
};

std::unordered_map<std::string, Sample *> *g_cachePtr = nullptr;
#define g_cache (*g_cachePtr)
std::size_t g_cacheBytes = 0;

struct Voice
{
    volatile int active = 0;
    const Sample *sample = nullptr;
    std::uint32_t pos = 0;  // 16.16 source position
    std::uint32_t step = 0; // 16.16 source frames per output frame
    int gain = 0;           // 0..256
};
Voice g_voices[kVoices];
int g_nextVoice = 0;

Sample *loadSample(const std::string &url)
{
    auto it = g_cache.find(url);
    if (it != g_cache.end())
        return it->second;

    if (g_cacheBytes > kMaxCacheBytes)
    {
        // Drop every sample no voice is playing.
        for (auto c = g_cache.begin(); c != g_cache.end();)
        {
            bool inUse = false;
            for (const Voice &v : g_voices)
                if (v.active && v.sample == c->second)
                    inUse = true;
            if (inUse)
            {
                ++c;
                continue;
            }
            g_cacheBytes -= c->second->pcm.size() * 2;
            delete c->second;
            c = g_cache.erase(c);
        }
    }

    unsigned int size = 0;
    unsigned char *data = AssetPak::isPakPath(url) ? AssetPak::load(AssetPak::keyOf(url), &size) : nullptr;
    if (!data)
        return nullptr;

    Sample *out = nullptr;
    Ps2AdpcmStream::Header hdr;
    if (Ps2AdpcmStream::parseHeader(data, size, hdr) && hdr.channels >= 1)
    {
        out = new Sample();
        out->rate = hdr.sampleRate;
        out->pcm.reserve(hdr.samplesPerChannel + 28);
        const std::uint8_t *blocks = data + Ps2AdpcmStream::kHeaderBytes;
        const std::size_t blockCount = (size - Ps2AdpcmStream::kHeaderBytes) / Ps2AdpcmStream::kBlockBytes;
        Ps2AdpcmStream::ChannelState st;
        std::int16_t tmp[Ps2AdpcmStream::kSamplesPerBlock];
        // Mono: blocks are sequential. (Stereo effects would interleave L/R blocks; use L only.)
        const std::size_t stride = static_cast<std::size_t>(hdr.channels);
        for (std::size_t b = 0; b < blockCount && out->pcm.size() < hdr.samplesPerChannel; b += stride)
        {
            if (!Ps2AdpcmStream::decodeBlock(blocks + b * Ps2AdpcmStream::kBlockBytes, st, tmp))
                break;
            const std::size_t room = hdr.samplesPerChannel - out->pcm.size();
            const std::size_t n = std::min<std::size_t>(room, Ps2AdpcmStream::kSamplesPerBlock);
            out->pcm.insert(out->pcm.end(), tmp, tmp + n);
        }
        if (out->pcm.empty())
        {
            delete out;
            out = nullptr;
        }
    }
    free(data);
    {
        static int traced = 0;
        if (traced < 12) { ++traced; PspSystem::bootTrace("audio: sample %s -> %s", url.c_str(), out ? "ok" : "FAIL"); }
    }
    if (!out)
        return nullptr;
    g_cacheBytes += out->pcm.size() * 2;
    g_cache[url] = out;
    return out;
}

void startVoice(const Sample *s, float volume)
{
    if (!g_started || !s || volume <= 0.0f)
        return;
    Voice *v = nullptr;
    for (int i = 0; i < kVoices; ++i)
        if (!g_voices[i].active) { v = &g_voices[i]; break; }
    if (!v)
    {
        v = &g_voices[g_nextVoice];
        g_nextVoice = (g_nextVoice + 1) % kVoices;
        v->active = 0;
    }
    v->sample = s;
    v->pos = 0;
    v->step = static_cast<std::uint32_t>((static_cast<std::uint64_t>(s->rate) << 16) / kOutRate);
    v->gain = static_cast<int>(std::min(1.0f, volume) * 256.0f);
    __sync_synchronize();
    v->active = 1;
}

// --------------------------------------------------------------------------------- music ---

struct MusicStream
{
    std::string archive;
    std::uint32_t offset = 0, size = 0;
    SceUID fd = -1; // opened by the game thread (path resolution is not thread-safe here)
    unsigned char *ring = nullptr;
    volatile std::uint32_t rd = 0, wr = 0; // byte counters (monotonic)
    volatile int eof = 0, stop = 0, mixerDone = 0;
    std::uint32_t frac = 0;   // 16.16 position between music frames
    std::int16_t last[2] = {0, 0}, next[2] = {0, 0};
    bool primed = false;
};

MusicStream *volatile g_musicCur = nullptr;  // owned by the mixer thread
MusicStream *volatile g_musicNext = nullptr; // handed over by the game thread
volatile int g_musicRunning = 0;             // 1 from start until the reader thread has finished

int musicReaderThread(SceSize, void *argp)
{
    MusicStream *ms = *static_cast<MusicStream **>(argp);
    SceUID fd = ms->fd;
    std::uint32_t filePos = 0;
    if (fd >= 0)
    {
        sceIoLseek32(fd, static_cast<int>(ms->offset), PSP_SEEK_SET);
        while (!ms->stop && filePos < ms->size)
        {
            const std::uint32_t used = ms->wr - ms->rd;
            if (kRingBytes - used < kReadChunk)
            {
                sceKernelDelayThread(15000);
                continue;
            }
            std::uint32_t want = std::min(kReadChunk, ms->size - filePos);
            want &= ~3u;
            if (!want) break;
            const std::uint32_t at = ms->wr % kRingBytes;
            want = std::min(want, kRingBytes - at);
            const int got = sceIoRead(fd, ms->ring + at, want);
            if (got <= 0) break;
            filePos += static_cast<std::uint32_t>(got);
            __sync_synchronize();
            ms->wr += static_cast<std::uint32_t>(got) & ~3u;
        }
        sceIoClose(fd);
    }
    ms->eof = 1;
    // Wait until the mixer has let go of the ring, then free it.
    while (!ms->mixerDone && !ms->stop)
        sceKernelDelayThread(20000);
    if (ms->stop)
        while (!ms->mixerDone)
            sceKernelDelayThread(5000);
    free(ms->ring);
    delete ms;
    g_musicRunning = 0;
    return sceKernelExitDeleteThread(0);
}

bool startMusic(const std::string &url)
{
    if (!g_started || g_musicRunning || g_musicNext || !AssetPak::isPakPath(url))
        return false;
    std::uint32_t off = 0, sz = 0;
    if (!AssetPak::locate(AssetPak::keyOf(url), &off, &sz) || sz < 4096)
        return false;
    MusicStream *ms = new MusicStream();
    ms->archive = AssetPak::archivePath();
    ms->fd = sceIoOpen(ms->archive.c_str(), PSP_O_RDONLY, 0);
    PspSystem::bootTrace("audio: music %s fd=%d off=%u size=%u", url.c_str(), (int)ms->fd, off, sz);
    if (ms->fd < 0) { delete ms; return false; }
    ms->offset = off;
    ms->size = sz;
    ms->ring = static_cast<unsigned char *>(malloc(kRingBytes));
    if (!ms->ring) { sceIoClose(ms->fd); delete ms; return false; }
    g_musicRunning = 1;
    SceUID th = sceKernelCreateThread("music", musicReaderThread, 0x1a, 16 * 1024, THREAD_ATTR_USER, nullptr);
    if (th < 0)
    {
        g_musicRunning = 0;
        sceIoClose(ms->fd);
        free(ms->ring);
        delete ms;
        return false;
    }
    g_musicNext = ms;
    MusicStream *arg = ms;
    sceKernelStartThread(th, sizeof(arg), &arg);
    return true;
}

void stopMusic()
{
    MusicStream *cur = g_musicCur;
    if (cur) cur->stop = 1;
    MusicStream *nxt = g_musicNext;
    if (nxt) nxt->stop = 1;
}

// --------------------------------------------------------------------------------- mixer ---

std::int16_t *g_out = nullptr;  // memalign(64), allocated when the mixer starts
std::int32_t *g_mix = nullptr;

bool musicPull(MusicStream *m, std::int16_t out[2])
{
    if (m->wr - m->rd < 4)
        return false;
    const std::uint32_t at = m->rd % kRingBytes;
    const std::int16_t *p = reinterpret_cast<const std::int16_t *>(m->ring + at);
    out[0] = p[0];
    out[1] = p[1];
    m->rd += 4;
    return true;
}

int mixerThread(SceSize, void *)
{
    // Plain 44.1 kHz stereo hardware channel (the SRC channel is not reliable at 22.05 kHz):
    // we mix at 22.05 kHz and duplicate/interpolate each frame into two output frames.
    const int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kBlockFrames * 2, PSP_AUDIO_FORMAT_STEREO);
    PspSystem::bootTrace("audio: channel reserve -> %d", ch);
    if (ch < 0)
        return sceKernelExitDeleteThread(0);
    bool tracedOut = false;
    std::int16_t prevL = 0, prevR = 0;
    for (;;)
    {
        std::memset(g_mix, 0, sizeof(std::int32_t) * kBlockFrames * 2);

        for (int vi = 0; vi < kVoices; ++vi)
        {
            Voice &v = g_voices[vi];
            if (!v.active) continue;
            const Sample *s = v.sample;
            const std::uint32_t len = static_cast<std::uint32_t>(s->pcm.size());
            std::uint32_t pos = v.pos;
            const std::int16_t *pcm = s->pcm.data();
            int i = 0;
            for (; i < kBlockFrames; ++i)
            {
                const std::uint32_t idx = pos >> 16;
                if (idx >= len) { v.active = 0; break; }
                const int sample = (pcm[idx] * v.gain) >> 8;
                g_mix[i * 2] += sample;
                g_mix[i * 2 + 1] += sample;
                pos += v.step;
            }
            v.pos = pos;
        }

        // Music
        MusicStream *m = g_musicCur;
        if (m && (m->stop || (m->eof && m->wr == m->rd)))
        {
            m->mixerDone = 1;
            g_musicCur = m = nullptr;
        }
        if (!m && g_musicNext)
        {
            g_musicCur = m = g_musicNext;
            g_musicNext = nullptr;
        }
        if (m)
        {
            const int gain = static_cast<int>(std::min(1.0f, g_musicVolume) * 200.0f);
            for (int i = 0; i < kBlockFrames; ++i)
            {
                // 22.05 kHz source straight into a 22.05 kHz output: one frame per output frame.
                std::int16_t f[2];
                if (!musicPull(m, f)) break;
                g_mix[i * 2] += (f[0] * gain) >> 8;
                g_mix[i * 2 + 1] += (f[1] * gain) >> 8;
            }
        }

        for (int i = 0; i < kBlockFrames; ++i)
        {
            int l = g_mix[i * 2], r = g_mix[i * 2 + 1];
            l = l > 32767 ? 32767 : (l < -32768 ? -32768 : l);
            r = r > 32767 ? 32767 : (r < -32768 ? -32768 : r);
            if (!tracedOut && (l > 64 || l < -64))
            {
                tracedOut = true;
                PspSystem::bootTrace("audio: first audible block");
            }
            g_out[i * 4] = static_cast<std::int16_t>((prevL + l) >> 1);
            g_out[i * 4 + 1] = static_cast<std::int16_t>((prevR + r) >> 1);
            g_out[i * 4 + 2] = static_cast<std::int16_t>(l);
            g_out[i * 4 + 3] = static_cast<std::int16_t>(r);
            prevL = static_cast<std::int16_t>(l);
            prevR = static_cast<std::int16_t>(r);
        }
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, g_out);
    }
}
} // namespace

// ------------------------------------------------------------------------- SoundManager ---

SoundManager::SoundManager() : soundVolume(0), options(nullptr), ticksBeforeMusic(0) {}

void SoundManager::loadSoundSettings(GameSettings *gamesettings)
{
    soundPoolStreaming.motionX = false;
    options = gamesettings;
    g_soundVolume = gamesettings ? gamesettings->soundVolume : 1.0f;
    g_musicVolume = gamesettings ? gamesettings->musicVolume : 1.0f;
    if (!loaded)
        tryToSetLibraryAndCodecs();
    ticksBeforeMusic = rand.nextInt(600) + 200;
}

void SoundManager::tryToSetLibraryAndCodecs()
{
    if (g_started)
    {
        loaded = true;
        return;
    }
    g_listenerPtr = new AudioListenerState();
    g_cachePtr = new std::unordered_map<std::string, Sample *>();
    g_out = static_cast<std::int16_t *>(memalign(64, sizeof(std::int16_t) * kBlockFrames * 4));
    g_mix = static_cast<std::int32_t *>(memalign(64, sizeof(std::int32_t) * kBlockFrames * 2));
    if (!g_out || !g_mix)
    {
        PspSystem::bootTrace("audio: out of memory");
        return;
    }
    SceUID th = sceKernelCreateThread("audio", mixerThread, 0x18, 16 * 1024, THREAD_ATTR_USER, nullptr);
    if (th >= 0 && sceKernelStartThread(th, 0, nullptr) >= 0)
    {
        g_started = true;
        loaded = true;
        PspSystem::bootTrace("audio: mixer started");
    }
    else
        PspSystem::bootTrace("audio: mixer failed to start (%d)", th);
}

void SoundManager::onSoundOptionsChanged()
{
    if (!options) return;
    g_soundVolume = options->soundVolume;
    g_musicVolume = options->musicVolume;
    if (g_musicVolume <= 0.0f)
        stopMusic();
}

void SoundManager::closeMinecraft() { stopMusic(); }

void SoundManager::addSound(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".adp"))
        soundPoolSounds.addSound(s, file);
}

void SoundManager::addStreaming(const jstring &, const std::string &) {} // music discs: not shipped on PSP

void SoundManager::addMusic(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".pcm"))
        soundPoolMusic.addSound(s, file);
}

void SoundManager::playRandomMusicIfReady()
{
    if (!g_started || !options || options->musicVolume <= 0.0f)
        return;
    if (g_musicRunning)
        return;
    if (ticksBeforeMusic > 0)
    {
        ticksBeforeMusic--;
        return;
    }
    SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
    ticksBeforeMusic = rand.nextInt(1200) + 600;
    if (entry)
        startMusic(entry->soundUrl);
}

bool SoundManager::playMusicFileNow(const std::string &file)
{
    if (!g_started || !options || options->musicVolume <= 0.0f)
        return false;
    return startMusic(file);
}

void SoundManager::setListenerPosition(EntityLiving *entityliving, float partialTick)
{
    if (!g_listenerPtr) return;
    updateAudioListener(g_listener, entityliving, partialTick);
}

void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}

void SoundManager::playSound(const jstring &s, float x, float y, float z, float volume, float)
{
    if (!g_started || !options || options->soundVolume <= 0.0f || volume <= 0.0f)
        return;
    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (!entry)
        return;
    const float att = audioSpatialAttenuation(g_listener, x, y, z, volume);
    if (att <= 0.0f)
        return;
    startVoice(loadSample(entry->soundUrl), std::min(volume, 1.0f) * att * g_soundVolume);
}

void SoundManager::playSoundFX(const jstring &s, float volume, float)
{
    if (!g_started || !options || options->soundVolume <= 0.0f)
        return;
    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry)
        startVoice(loadSample(entry->soundUrl), std::min(volume, 1.0f) * 0.5f * g_soundVolume);
}

#endif // PSP_NO_AUDIO
