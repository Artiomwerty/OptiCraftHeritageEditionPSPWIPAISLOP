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
volatile unsigned g_pspAudioBlocks = 0;
volatile int g_pspAudioLastError = 0;
volatile int g_pspAudioVoices = 0;
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
constexpr int kBlockFrames = 1536; // mix frames per block (multiple of 64): 3072 output frames = 70 ms
constexpr int kVoices = 8;
constexpr std::size_t kMaxCacheBytes = 3 * 1024 * 1024;
constexpr std::uint32_t kRingBytes = 128 * 1024;
constexpr std::uint32_t kReadChunk = 16 * 1024;

AudioListenerState *g_listenerPtr = nullptr;
#define g_listener (*g_listenerPtr)
float g_soundVolume = 1.0f;
float g_musicVolume = 1.0f;
bool g_started = false;
} // namespace
// Mixer heartbeat for the perf line in boot.log: blocks handed to the hardware, last output error.
volatile unsigned g_pspAudioBlocks = 0;
volatile int g_pspAudioLastError = 0;
volatile int g_pspAudioVoices = 0;
namespace
{

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
    SceUID fd = -1; // opened by the game thread (path resolution is not thread-safe here)
    std::uint32_t offset = 0, size = 0, filePos = 0;
    unsigned char *ring = nullptr;
    std::uint32_t rd = 0, wr = 0; // byte counters (monotonic), touched only by the mixer once handed over
    volatile int stop = 0;
    bool eof = false;
    bool primed = false;
};

std::string g_pendingDisc;
bool g_discActive = false;
MusicStream *volatile g_musicCur = nullptr;  // owned by the mixer thread
MusicStream *volatile g_musicNext = nullptr; // handed over by the game thread
volatile int g_musicRunning = 0;             // 1 from start until the mixer has released the stream

void musicRelease(MusicStream *m)
{
    if (m->fd >= 0) sceIoClose(m->fd);
    free(m->ring);
    delete m;
    g_musicRunning = 0;
}

// Reads from the Memory Stick on the mixer thread itself, a little each block: no second thread
// to be starved or to starve the game, and the schedule is deterministic.
void musicRefill(MusicStream *m, std::uint32_t maxBytes)
{
    if (m->eof || m->fd < 0) return;
    while (maxBytes >= 4096)
    {
        const std::uint32_t used = m->wr - m->rd;
        if (kRingBytes - used < 8192 || m->filePos >= m->size) break;
        std::uint32_t want = std::min<std::uint32_t>(8192, m->size - m->filePos);
        want &= ~3u;
        if (!want) { m->eof = true; break; }
        const std::uint32_t at = m->wr % kRingBytes;
        want = std::min(want, kRingBytes - at);
        const std::uint64_t rt0 = PspSystem::micros();
        const int got = sceIoRead(m->fd, m->ring + at, want);
        {
            static int slowLogged = 0;
            const std::uint64_t dt = PspSystem::micros() - rt0;
            if (dt > 20000 && slowLogged < 10) { ++slowLogged; PspSystem::bootTrace("audio: slow music read %u us", (unsigned)dt); }
        }
        if (got <= 0) { m->eof = true; break; }
        m->filePos += static_cast<std::uint32_t>(got);
        m->wr += static_cast<std::uint32_t>(got) & ~3u;
        if (m->filePos >= m->size) m->eof = true;
        maxBytes -= std::min<std::uint32_t>(maxBytes, static_cast<std::uint32_t>(got));
    }
}

// Extra music packs: musicN.pak files next to assets.pak (same MCPK format, music entries only), so the
// big soundtrack can be installed as several small downloads. Entries are addressed as "pakx://<key>".
struct ExtraEntry { std::string path; std::uint32_t off; std::uint32_t size; };
std::unordered_map<std::string, ExtraEntry> g_extra;

std::uint32_t be32(const unsigned char *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

void scanExtraPak(const std::string &path, std::vector<std::pair<std::string, std::string>> &music)
{
    const SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return;
    unsigned char h[32];
    if (sceIoRead(fd, h, 32) == 32 && std::memcmp(h, "MCPK", 4) == 0 && be32(h + 4) == 1)
    {
        const std::uint32_t count = be32(h + 8), tableOff = be32(h + 12), namesOff = be32(h + 16), namesBytes = be32(h + 20);
        if (count > 0 && count < 4096 && namesBytes < (1u << 20))
        {
            std::vector<unsigned char> table(count * 16), names(namesBytes + 1, 0);
            sceIoLseek32(fd, static_cast<int>(tableOff), PSP_SEEK_SET);
            const bool okT = sceIoRead(fd, table.data(), static_cast<SceSize>(table.size())) == static_cast<int>(table.size());
            sceIoLseek32(fd, static_cast<int>(namesOff), PSP_SEEK_SET);
            const bool okN = sceIoRead(fd, names.data(), namesBytes) == static_cast<int>(namesBytes);
            if (okT && okN)
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const unsigned char *e = table.data() + i * 16;
                    const std::uint32_t nameOff = be32(e + 4), dataOff = be32(e + 8), size = be32(e + 12);
                    if (nameOff >= namesBytes) continue;
                    const std::string key(reinterpret_cast<const char *>(names.data() + nameOff));
                    if (key.compare(0, 10, "resources/") != 0 || key.size() < 5 || key.compare(key.size() - 4, 4, ".pcm") != 0)
                        continue;
                    if (AssetPak::exists(key)) continue; // already in the main pak
                    const std::string url = "pakx://" + key;
                    g_extra[url] = ExtraEntry{path, dataOff, size};
                    if (key.compare(0, 20, "resources/streaming/") != 0)
                        music.emplace_back(key, url);
                }
        }
    }
    sceIoClose(fd);
}

void scanExtraPaks(std::vector<std::pair<std::string, std::string>> &music)
{
    std::string arch = AssetPak::archivePath();
    const std::size_t slash = arch.find_last_of("/:");
    if (arch.empty() || slash == std::string::npos) return;
    const std::string dir = arch.substr(0, slash + 1);
    const SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return;
    SceIoDirent ent;
    std::memset(&ent, 0, sizeof(ent));
    while (sceIoDread(d, &ent) > 0)
    {
        std::string n = ent.d_name;
        std::string low;
        for (char c : n) low += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        if (low.size() > 9 && low.compare(0, 5, "music") == 0 && low.compare(low.size() - 4, 4, ".pak") == 0)
        {
            scanExtraPak(dir + n, music);
            PspSystem::bootTrace("audio: extra pak %s", n.c_str());
        }
        std::memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
}

bool startMusic(const std::string &url)
{
    if (!g_started || g_musicRunning || g_musicNext)
        return false;
    std::uint32_t off = 0, sz = 0;
    std::string archive;
    const auto ex = g_extra.find(url);
    if (ex != g_extra.end())
    {
        archive = ex->second.path;
        off = ex->second.off;
        sz = ex->second.size;
    }
    else
    {
        if (!AssetPak::isPakPath(url) || !AssetPak::locate(AssetPak::keyOf(url), &off, &sz))
            return false;
        archive = AssetPak::archivePath();
    }
    if (sz < 4096)
        return false;
    MusicStream *ms = new MusicStream();
    ms->fd = sceIoOpen(archive.c_str(), PSP_O_RDONLY, 0);
    PspSystem::bootTrace("audio: music %s fd=%d off=%u size=%u", url.c_str(), (int)ms->fd, off, sz);
    if (ms->fd < 0) { delete ms; return false; }
    sceIoLseek32(ms->fd, static_cast<int>(off), PSP_SEEK_SET);
    ms->offset = off;
    ms->size = sz;
    ms->ring = static_cast<unsigned char *>(malloc(kRingBytes));
    if (!ms->ring) { sceIoClose(ms->fd); delete ms; return false; }
    g_musicRunning = 1;
    __sync_synchronize();
    g_musicNext = ms;
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
    // The shipped .pcm tracks are MONO 22.05 kHz 16-bit (measured: adjacent samples are smooth, and
    // piano1 runs 68 s = the real "Dry Hands"). Reading them as stereo played at double speed.
    if (m->wr - m->rd < 2)
        return false;
    const std::uint32_t at = m->rd % kRingBytes;
    const std::int16_t s = *reinterpret_cast<const std::int16_t *>(m->ring + at);
    out[0] = s;
    out[1] = s;
    m->rd += 2;
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
            g_musicCur = nullptr;
            musicRelease(m);
            m = nullptr;
        }
        if (!m && g_musicNext)
        {
            m = g_musicNext;
            g_musicNext = nullptr;
            if (m->stop) { musicRelease(m); m = nullptr; }
            else g_musicCur = m;
        }
        if (m)
        {
            if (!m->primed)
            {
                // Fill most of the ring before the first sample so playback never starts dry.
                for (int k = 0; k < 6 && !m->eof && (m->wr - m->rd) < 48 * 1024; ++k)
                    musicRefill(m, 8192);
                m->primed = true;
            }
            else
                musicRefill(m, 8192);
            const int gain = static_cast<int>(std::min(1.0f, g_musicVolume) * 200.0f);
            for (int i = 0; i < kBlockFrames; ++i)
            {
                // 22.05 kHz source straight into the 22.05 kHz mix: one frame per mix frame.
                std::int16_t f[2];
                if (!musicPull(m, f))
                {
                    static int dry = 0;
                    if (dry < 10 && !m->eof) { ++dry; PspSystem::bootTrace("audio: music ring dry (rd=%u wr=%u)", m->rd, m->wr); }
                    break;
                }
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
        sceKernelDcacheWritebackRange(g_out, sizeof(std::int16_t) * kBlockFrames * 4);
        {
            int nv = 0;
            for (int vi = 0; vi < kVoices; ++vi) nv += g_voices[vi].active ? 1 : 0;
            g_pspAudioVoices = nv;
        }
        const int orc = sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, g_out);
        ++g_pspAudioBlocks;
        if (orc < 0)
        {
            // Never spin at this priority: an output error would starve the game.
            static int errLogged = 0;
            g_pspAudioLastError = orc;
            if (errLogged < 6) { ++errLogged; PspSystem::bootTrace("audio: output error 0x%08x", (unsigned)orc); }
            sceKernelDelayThread(20000);
        }
        {
            // Gap diagnostics: how late the next block was handed over.
            static std::uint64_t lastT = 0;
            static int lateLogged = 0, blocks = 0;
            const std::uint64_t now = PspSystem::micros();
            ++blocks;
            if (lastT && now - lastT > 90000 && lateLogged < 12 && blocks > 8)
            {
                ++lateLogged;
                PspSystem::bootTrace("audio: late block %u us", (unsigned)(now - lastT));
            }
            lastT = now;
        }
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
        {
            std::vector<std::pair<std::string, std::string>> extra;
            scanExtraPaks(extra);
            for (const auto &e : extra)
                soundPoolMusic.addSound(e.first, e.second);
            PspSystem::bootTrace("audio: %d extra music tracks", (int)extra.size());
        }
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
    // Menu and world both call this every tick. A track starts right away on the title screen; after
    // any track ends the next one waits two minutes (wall-clock, independent of the frame rate).
    static int prevRunning = 0;
    static std::uint64_t nextAllowedUs = 0;
    const int running = g_musicRunning;
    const std::uint64_t now = PspSystem::micros();
    if (prevRunning && !running)
        nextAllowedUs = now + 120ull * 1000000ull;
    prevRunning = running;

    if (!running)
        g_discActive = false;
    if (!g_pendingDisc.empty() && !running && !g_musicNext && g_started)
    {
        g_discActive = startMusic(g_pendingDisc);
        g_pendingDisc.clear();
        return;
    }
    if (!g_pendingDisc.empty() || g_discActive)
        return;
    if (options) g_musicVolume = options->musicVolume;
    if (!g_started || !options || options->musicVolume <= 0.0f)
        return;
    if (running || now < nextAllowedUs)
        return;
    SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
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

void SoundManager::playStreaming(const jstring &name, float, float, float, float, float)
{
    // Music discs (jukebox): stop whatever plays, then stream resources/streaming/<name>.pcm.
    if (!g_started) return;
    if (name.empty())
    {
        g_pendingDisc.clear();
        if (g_discActive) stopMusic();
        return;
    }
    const std::string key = "resources/streaming/" + std::string(name) + ".pcm";
    std::string url = "pakx://" + key;
    if (g_extra.find(url) == g_extra.end())
    {
        if (!AssetPak::exists(key)) return;
        url = AssetPak::makePath(key);
    }
    g_pendingDisc = url;
    stopMusic();
}

void SoundManager::playSound(const jstring &s, float x, float y, float z, float volume, float)
{
    if (!g_started || !options || options->soundVolume <= 0.0f || volume <= 0.0f)
        return;
    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (!entry)
        return;
    g_soundVolume = options->soundVolume;
    const float att = audioSpatialAttenuation(g_listener, x, y, z, volume);
    if (att <= 0.0f)
        return;
    startVoice(loadSample(entry->soundUrl), std::min(volume, 1.0f) * att * g_soundVolume);
}

void SoundManager::playSoundFX(const jstring &s, float volume, float)
{
    if (!g_started || !options || options->soundVolume <= 0.0f)
        return;
    g_soundVolume = options->soundVolume;
    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry)
    {
        static int traced = 0;
        if (traced < 3) { ++traced; PspSystem::bootTrace("audio: fx vol=%.2f sound=%.2f music=%.2f blocks=%u", (double)volume, (double)options->soundVolume, (double)options->musicVolume, (unsigned)g_pspAudioBlocks); }
        startVoice(loadSample(entry->soundUrl), std::min(volume, 1.0f) * 0.5f * g_soundVolume);
    }
}

#endif // PSP_NO_AUDIO
