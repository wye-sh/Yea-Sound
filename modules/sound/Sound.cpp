// The Sound component's main-thread side: the registry of every sound games declared, the policy of each
// play (cooldown, instance limit and stealing, variations without repeats, jitter), streamed files decoded
// into rings, the device, and the voices kept across a swap of this module's own code. The mixer is
// Mixer.cpp.
//
// Exports run inside the calling module's guard and heap (Yea charges a call to its caller), so nothing
// here allocates through the host: every buffer is this module's own malloc, freed in unload() at the
// latest.
#include "yea_sound.h"

#include "Miniaudio.h"
#include "Mixer.h"
#include "SoundLink.h"

#include <algorithm>
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sound
{
void nap(unsigned ms); // Codec.cpp
#if defined(_WIN32)
void sessionBegin();               // Session.cpp: the session Windows' mixer keeps its level under
void sessionName(void* audioClient);
#endif

namespace
{
using namespace mix;

constexpr uint32_t kClips = 2048;
constexpr uint32_t kVariations = 32;
constexpr uint32_t kPeaks = 256;
constexpr float    kStreamSeconds = 10.0f; // files this long or longer stream; shorter ones are decoded whole

double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uint64_t fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

void copy(char* to, const char* from, size_t bytes)
{
    snprintf(to, bytes, "%s", from ? from : "");
}

// ---- Clips: one file's content, shared by every sound with the same bytes ----

struct Clip
{
    uint64_t       hash, size;
    int            refs;          // defs holding it
    bool           streamed;
    Buffer         buffer;        // decoded, when not streamed
    unsigned char* bytes;         // the file, when streamed (the decoder reads it)
    uint32_t       channels, rate;
    float          seconds;
    float          peaks[kPeaks * 2];
    bool           peaksReady;
};
Clip* gClips[kClips];

void freeClip(uint32_t i)
{
    Clip* c = gClips[i];
    free(c->buffer.frames);
    free(c->bytes);
    free(c);
    gClips[i] = nullptr;
}

bool openDecoder(const void* bytes, size_t size, uint32_t channels, ma_decoder* d)
{
    ma_decoder_config c = ma_decoder_config_init(ma_format_f32, channels, 0);
    if (ma_decoder_init_memory(bytes, size, &c, d) != MA_SUCCESS)
        return false;
    if (d->outputChannels <= 2)
        return true;
    ma_decoder_uninit(d); // surround: down to stereo
    c = ma_decoder_config_init(ma_format_f32, 2, 0);
    return ma_decoder_init_memory(bytes, size, &c, d) == MA_SUCCESS;
}

void addPeaks(Clip* c, const float* frames, uint64_t count, uint64_t first, uint64_t total)
{
    for (uint64_t i = 0; i < count; ++i)
    {
        const uint64_t k = std::min<uint64_t>(kPeaks - 1, (first + i) * kPeaks / std::max<uint64_t>(1, total));
        for (uint32_t ch = 0; ch < c->channels; ++ch)
        {
            const float x = frames[i * c->channels + ch];
            c->peaks[2 * k] = std::min(c->peaks[2 * k], x);
            c->peaks[2 * k + 1] = std::max(c->peaks[2 * k + 1], x);
        }
    }
}

// The clip for a file's bytes (made now when new): decoded whole, or copied to stream when long.
Clip* clipFor(const unsigned char* bytes, uint64_t size, int streamMode, bool music, char* error, uint32_t errorBytes)
{
    const uint64_t hash = fnv(bytes, size);
    int            slot = -1;
    for (uint32_t i = 0; i < kClips; ++i)
    {
        if (gClips[i] && gClips[i]->hash == hash && gClips[i]->size == size)
            return gClips[i];
        if (!gClips[i] && slot < 0)
            slot = int(i);
    }
    if (slot < 0)
        return snprintf(error, errorBytes, "too many sound files at once (%u)", kClips), nullptr;
    ma_decoder d;
    if (!openDecoder(bytes, size, 0, &d))
        return snprintf(error, errorBytes, "can't decode it (Ogg Vorbis, WAV or FLAC; not MP3)"), nullptr;
    ma_uint64 length = 0;
    ma_decoder_get_length_in_pcm_frames(&d, &length);
    Clip* c = (Clip*)calloc(1, sizeof(Clip));
    if (!c)
        return ma_decoder_uninit(&d), snprintf(error, errorBytes, "out of memory"), nullptr;
    gClips[slot] = c; // from here, freed by collect() should this define fail
    c->hash = hash, c->size = size, c->channels = d.outputChannels, c->rate = d.outputSampleRate;
    c->seconds = float(double(length) / std::max(1u, c->rate));
    for (uint32_t k = 0; k < kPeaks; ++k)
        c->peaks[2 * k] = 0, c->peaks[2 * k + 1] = 0;
    c->streamed = streamMode == 1 || (streamMode == 0 && (music || c->seconds >= kStreamSeconds));
    if (c->streamed)
    {
        ma_decoder_uninit(&d);
        c->bytes = (unsigned char*)malloc(size);
        if (!c->bytes)
            return snprintf(error, errorBytes, "out of memory"), nullptr;
        memcpy(c->bytes, bytes, size);
        return c;
    }
    // Decoded whole, growing as it goes (a length the file doesn't know is fine).
    uint64_t cap = length ? length : c->rate, count = 0;
    float*   frames = (float*)malloc(cap * c->channels * sizeof(float));
    for (;;)
    {
        if (!frames)
            return ma_decoder_uninit(&d), snprintf(error, errorBytes, "out of memory"), nullptr;
        if (count == cap)
        {
            cap *= 2;
            float* grown = (float*)realloc(frames, cap * c->channels * sizeof(float));
            if (!grown)
                free(frames);
            frames = grown;
            continue;
        }
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&d, frames + count * c->channels, cap - count, &got);
        count += got;
        if (got == 0)
            break;
    }
    ma_decoder_uninit(&d);
    if (!count)
        return free(frames), snprintf(error, errorBytes, "it has no sound in it"), nullptr;
    c->buffer = Buffer{ frames, uint32_t(count), c->channels, c->rate };
    c->seconds = float(double(count) / c->rate);
    addPeaks(c, frames, count, 0, count);
    c->peaksReady = true;
    return c;
}

// ---- Sounds ----

struct Opts
{
    uint32_t bus = BusEffects, max = 8, steal = 0, priority = 1, cut = CutFade, switchAt = SwitchBar, beats = 4;
    bool     loop = false, music = false;
    int      stream = 0; // 0 by length, 1 stream, 2 decode whole
    float    jitterPitch = 0, jitterVolume = 0, cooldown = 0, cutFade = 0.05f, fadeIn = 0, nearDist = 2, farDist = 40;
    float    bpm = 120, crossfade = 1;
};
enum : uint32_t { StealAuto, StealOldest, StealFarthest, StealQuietest, StealNone };

struct Def
{
    bool     used, retired, confirmed;
    char     owner[64], name[64], options[256];
    uint64_t key; // owner, name, options and every file's content: the same key is the same sound
    Opts     o;
    Clip*    variations[kVariations];
    uint32_t variationCount;
    Clip*    intro;
    Clip*    outro;
    float    volumeDb, pitch;
    double   lastPlay;
    uint32_t recent[8], recentCount; // the last variations played, newest first
    uint64_t order;                  // when it was first defined
};
Def      gDefs[kDefs];
uint64_t gOrderNext = 1;
uint32_t gOrder[kDefs], gOrderCount = 0; // live defs by `order` (the registry a panel lists)
bool     gOrderDirty = true;

// Parses a sound's options; false with why.
bool parse(const char* text, bool hasIntroOrOutro, Opts* o, char* error, uint32_t errorBytes)
{
    char words[64][24];
    int  n = 0;
    for (const char* p = text ? text : ""; *p && n < 64;)
    {
        while (*p == ' ' || *p == '\t')
            ++p;
        int k = 0;
        while (*p && *p != ' ' && *p != '\t')
        {
            if (k < 23)
                words[n][k++] = *p;
            ++p;
        }
        if (k)
            words[n][k] = 0, ++n;
    }
    bool  set[32] = {};
    auto  num = [&](int i, float* out) {
        char* end = nullptr;
        if (i >= n)
            return false;
        *out = strtof(words[i], &end);
        return end && *end == 0;
    };
    auto  pick = [&](int i, const char* const* names, int count, uint32_t* out) {
        for (int k = 0; i < n && k < count; ++k)
            if (!strcmp(words[i], names[k]))
                return *out = uint32_t(k), true;
        return false;
    };
    static const char* const kBuses[] = { "effects", "interface", "voice", "ambience", "music" };
    static const char* const kSteal[] = { "auto", "oldest", "farthest", "quietest", "none" };
    static const char* const kPriority[] = { "low", "normal", "high" };
    static const char* const kStop[] = { "default", "now", "fade", "outro", "release" };
    static const char* const kSwitch[] = { "bar", "beat", "now", "end" };
    enum { SBus, SJitter, SCooldown, SPriority, SStop, SMax, SLoop };
    for (int i = 0; i < n; ++i)
    {
        const char* w = words[i];
        float       f = 0, g = 0;
        uint32_t    u = 0;
        bool        ok = true;
        if (!strcmp(w, "loop"))
            o->loop = true, set[SLoop] = true;
        else if (!strcmp(w, "once"))
            o->loop = false, set[SLoop] = true;
        else if (!strcmp(w, "music"))
            o->music = true;
        else if (!strcmp(w, "stream"))
            o->stream = 1;
        else if (!strcmp(w, "load"))
            o->stream = 2;
        else if (!strcmp(w, "bus") && (ok = pick(i + 1, kBuses, 5, &u)))
            o->bus = u, set[SBus] = true, ++i;
        else if (!strcmp(w, "jitter") && (ok = num(i + 1, &f) && num(i + 2, &g) && f >= 0 && g >= 0))
            o->jitterPitch = f, o->jitterVolume = g, set[SJitter] = true, i += 2;
        else if (!strcmp(w, "max") && (ok = num(i + 1, &f) && f >= 1 && f <= 256))
            o->max = uint32_t(f), set[SMax] = true, ++i;
        else if (!strcmp(w, "steal") && (ok = pick(i + 1, kSteal, 5, &u)))
            o->steal = u, ++i;
        else if (!strcmp(w, "cooldown") && (ok = num(i + 1, &f) && f >= 0 && f <= 60))
            o->cooldown = f, set[SCooldown] = true, ++i;
        else if (!strcmp(w, "priority") && (ok = pick(i + 1, kPriority, 3, &u)))
            o->priority = u, set[SPriority] = true, ++i;
        else if (!strcmp(w, "range") && (ok = num(i + 1, &f) && num(i + 2, &g) && f >= 0 && g > f))
            o->nearDist = f, o->farDist = g, i += 2;
        else if (!strcmp(w, "stop") && (ok = pick(i + 1, kStop, 5, &u) && u != CutDefault))
        {
            o->cut = u, set[SStop] = true, ++i;
            if (u == CutFade && (ok = num(i + 1, &f) && f >= 0 && f <= 60))
                o->cutFade = f, ++i;
        }
        else if (!strcmp(w, "fadein") && (ok = num(i + 1, &f) && f >= 0 && f <= 60))
            o->fadeIn = f, ++i;
        else if (!strcmp(w, "bpm") && (ok = num(i + 1, &f) && f >= 20 && f <= 400))
        {
            o->bpm = f, ++i;
            if (num(i + 1, &g) && g >= 1 && g <= 16)
                o->beats = uint32_t(g), ++i;
        }
        else if (!strcmp(w, "switch") && (ok = pick(i + 1, kSwitch, 4, &u)))
        {
            o->switchAt = u, ++i;
            if (num(i + 1, &f) && f >= 0 && f <= 30)
                o->crossfade = f, ++i;
        }
        else
            ok = false;
        if (!ok)
        {
            snprintf(error, errorBytes,
                     "can't read \"%s\" in its options (known: loop, once, music, bus effects|interface|voice|ambience|music, "
                     "jitter <semitones> <dB>, max <n>, steal oldest|farthest|quietest|none, cooldown <s>, priority low|normal|high, "
                     "range <near> <far>, stop now|fade <s>|outro|release, fadein <s>, stream, load, bpm <n> [<beats>], "
                     "switch bar|beat|now|end [<s>])",
                     w);
            return false;
        }
    }
    // The defaults that depend on what it is.
    if (o->music)
    {
        if (!set[SLoop])
            o->loop = true;
        if (!set[SBus])
            o->bus = BusMusic;
        if (!set[SPriority])
            o->priority = 2;
    }
    if (hasIntroOrOutro && !set[SLoop])
        o->loop = true;
    if (o->bus == BusVoice && !set[SPriority])
        o->priority = 2;
    if (!set[SStop])
        o->cut = o->loop ? (hasIntroOrOutro ? CutOutro : CutFade) : CutFade, o->cutFade = o->loop ? 0.15f : 0.05f;
    const bool oneShotEffect = !o->loop && !o->music && (o->bus == BusEffects || o->bus == BusAmbience);
    if (!set[SJitter] && oneShotEffect)
        o->jitterPitch = 0.5f, o->jitterVolume = 1.5f;
    if (!set[SCooldown] && !o->loop && !o->music)
        o->cooldown = 0.03f;
    return true;
}

void releaseDef(uint32_t i)
{
    Def& d = gDefs[i];
    for (uint32_t k = 0; k < d.variationCount; ++k)
        d.variations[k]->refs--;
    if (d.intro)
        d.intro->refs--;
    if (d.outro)
        d.outro->refs--;
    memset(&d, 0, sizeof(d));
    gOrderDirty = true;
}

// ---- Voices, as the main thread knows them ----

struct Mirror
{
    bool     used, positional, stopping, preview;
    uint32_t gen, def, variation;
    double   started;
    float    at[3];
};
Mirror   gMirror[kVoices];
uint64_t gRandom = 0x9e3779b97f4a7c15ull;
float    gListener[3] = {};
uint32_t gMusic = 0, gMusicGen = 0, gMusicDef = 0; // the view's music voice (slot+1, gen) and sound (id)
snd::Voice    gPreview = 0;

float random01()
{
    gRandom ^= gRandom << 13, gRandom ^= gRandom >> 7, gRandom ^= gRandom << 17;
    return float(gRandom >> 40) / float(1 << 24);
}

bool alive(uint32_t slot) { return gMirror[slot].used && gDoneGen[slot].load(std::memory_order_acquire) != gMirror[slot].gen; }

bool valid(snd::Voice v, uint32_t* slot)
{
    if (!v)
        return false;
    *slot = v & 0xff;
    return gMirror[*slot].gen == (v >> 8) && alive(*slot);
}

float distanceTo(const float* at)
{
    const float dx = at[0] - gListener[0], dy = at[1] - gListener[1], dz = at[2] - gListener[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void stopVoices(uint32_t def, float seconds)
{
    for (uint32_t i = 0; i < kVoices; ++i)
        if (alive(i) && gMirror[i].def == def && !gMirror[i].stopping)
        {
            Command c = {};
            c.type = CmdStop, c.slot = i, c.gen = gMirror[i].gen, c.cut = CutFade, c.fade = seconds;
            if (push(c))
                gMirror[i].stopping = true;
        }
}

// ---- Streams: decoded on the main thread (in draw, which runs while paused too), read by the mixer ----

struct Decoding
{
    ma_decoder   decoder;
    bool         open;
    const Clip*  clip;
    bool         loop;
    uint64_t     seek; // where to start, in the file's frames
};
Decoding gDecoding[kStreams];

void topUp(uint32_t i)
{
    Stream&   st = gStreams[i];
    Decoding& dc = gDecoding[i];
    uint32_t  s = st.state.load(std::memory_order_acquire);
    if (s == StreamClose)
    {
        if (dc.open)
            ma_decoder_uninit(&dc.decoder), dc.open = false;
        dc.clip = nullptr;
        st.written = 0, st.read = 0, st.ended = ~0ull, st.wrapCount = 0;
        for (auto& w : st.wraps)
            w = 0;
        st.state.store(StreamFree, std::memory_order_release);
        return;
    }
    if (s == StreamOpen)
    {
        if (!dc.clip || !openDecoder(dc.clip->bytes, dc.clip->size, dc.clip->channels, &dc.decoder))
        {
            st.ended = 0, st.channels = 2, st.rate = 48000;
            st.state.store(StreamReady, std::memory_order_release); // ends at once
            return;
        }
        dc.open = true;
        if (dc.seek)
            ma_decoder_seek_to_pcm_frame(&dc.decoder, dc.seek);
        st.channels = dc.decoder.outputChannels, st.rate = dc.decoder.outputSampleRate;
        st.state.store(StreamReady, std::memory_order_release);
        s = StreamReady;
    }
    if (s != StreamReady || !dc.open)
        return;
    for (int guard = 0; guard < 64; ++guard)
    {
        const uint64_t w = st.written.load(std::memory_order_relaxed), r = st.read.load(std::memory_order_acquire);
        const uint64_t room = kRing - (w - r);
        if (room < 1024 || st.ended.load() != ~0ull)
            break;
        const uint64_t at = w % kRing;
        const uint64_t n = std::min<uint64_t>(std::min<uint64_t>(room, kRing - at), 8192);
        ma_uint64      got = 0;
        ma_decoder_read_pcm_frames(&dc.decoder, &st.frames[at * st.channels], n, &got);
        st.written.store(w + got, std::memory_order_release);
        if (got < n)
        {
            if (dc.loop && w + got > 0)
            {
                ma_decoder_seek_to_pcm_frame(&dc.decoder, 0);
                const uint32_t k = st.wrapCount.fetch_add(1);
                st.wraps[k % 4].store(w + got, std::memory_order_release);
            }
            else
            {
                st.ended.store(w + got, std::memory_order_release);
                break;
            }
        }
    }
}

// A streamed clip's peaks, a little each frame.
struct Scan
{
    Clip*      clip;
    ma_decoder decoder;
    uint64_t   done, total;
} gScan;

void scanPeaks()
{
    if (!gScan.clip)
    {
        for (uint32_t i = 0; i < kClips && !gScan.clip; ++i)
            if (gClips[i] && gClips[i]->streamed && !gClips[i]->peaksReady && gClips[i]->refs > 0)
            {
                Clip* c = gClips[i];
                if (!openDecoder(c->bytes, c->size, c->channels, &gScan.decoder))
                {
                    c->peaksReady = true; // nothing to draw
                    continue;
                }
                ma_uint64 total = 0;
                ma_decoder_get_length_in_pcm_frames(&gScan.decoder, &total);
                gScan.clip = c, gScan.done = 0, gScan.total = total ? total : uint64_t(c->seconds * c->rate);
            }
        if (!gScan.clip)
            return;
    }
    float     frames[4096 * 2];
    ma_uint64 got = 0;
    for (int k = 0; k < 6; ++k) // about half a second of a 48 kHz file per frame
    {
        ma_decoder_read_pcm_frames(&gScan.decoder, frames, 4096, &got);
        addPeaks(gScan.clip, frames, got, gScan.done, gScan.total);
        gScan.done += got;
        if (got < 4096)
            break;
    }
    if (got < 4096)
    {
        gScan.clip->peaksReady = true;
        ma_decoder_uninit(&gScan.decoder);
        gScan.clip = nullptr;
    }
}

void stopScan()
{
    if (gScan.clip)
        ma_decoder_uninit(&gScan.decoder), gScan.clip = nullptr;
}

// ---- Voices resumed after this module's code was swapped ----

struct Pending
{
    uint64_t key;
    uint32_t slot, gen, seg, variation, flags;
    double   pos;
    float    at[3], tuneDb, tunePitch;
};
Pending  gPending[kVoices];
uint32_t gPendingCount = 0;
double   gPendingUntil = 0; // given up on after this (their sound never came back)

uint64_t stamp() { return (uint64_t)(uintptr_t)yea::host(); } // this run of Yea's (the host moves between runs)

// ---- Playing ----

bool pending(uint32_t slot)
{
    for (uint32_t i = 0; i < gPendingCount; ++i)
        if (gPending[i].slot == slot)
            return true;
    return false;
}

// A free voice slot (one a resumed voice is waiting for is not); -1 when all 256 are taken.
int freeSlot()
{
    for (uint32_t i = 0; i < kVoices; ++i)
        if (!alive(i) && !pending(i))
            return int(i);
    return -1;
}

bool gLoaded = false;
bool gDevice = false; // a device runs

// Takes a stream slot for clip c, decoding from `seek`; -1 when all are busy.
int takeStream(const Clip* c, bool loop, uint64_t seek)
{
    for (uint32_t i = 0; i < kStreams; ++i)
    {
        uint32_t want = StreamFree;
        if (gStreams[i].state.compare_exchange_strong(want, StreamReserved))
        {
            gDecoding[i].clip = c, gDecoding[i].loop = loop, gDecoding[i].seek = seek;
            gStreams[i].state.store(StreamOpen, std::memory_order_release);
            return int(i);
        }
    }
    return -1;
}

Source sourceOf(const Clip* c) { return c && !c->streamed ? Source{ &c->buffer, -1 } : Source{}; }

// Sends a play for def `id` in `slot`. The caller has checked the limits.
snd::Voice start(uint32_t id, uint32_t slot, uint32_t gen, uint32_t variation, uint32_t flags, const float* at, float volume, float pitch,
            uint32_t seg, double pos, float fadeIn)
{
    Def&          d = gDefs[id - 1];
    const double  t = nowSeconds();
    Command       c = {};
    const bool    preview = flags & FlagPreview;
    Clip* const   body = d.variations[variation];
    c.type = CmdPlay, c.slot = slot, c.def = id - 1, c.gen = gen;
    c.bus = uint8_t(preview ? BusInterface : d.o.bus), c.priority = uint8_t(preview ? 2 : d.o.priority);
    c.cut = uint8_t(d.o.cut), c.fade = d.o.cut == CutFade ? d.o.cutFade : kRamp;
    c.flags = flags | (d.o.loop && !preview ? FlagLoop : 0) | (d.o.music && !preview ? FlagMusic : 0);
    c.volume = volume, c.pitch = pitch;
    if (at)
        memcpy(c.a, at, sizeof(c.a));
    c.fadeIn = std::max(fadeIn, d.o.fadeIn);
    c.nearDist = d.o.nearDist, c.farDist = d.o.farDist, c.bpm = d.o.bpm, c.beats = d.o.beats;
    c.switchAt = d.o.switchAt, c.crossfade = d.o.crossfade;
    c.music = gMusic && alive(gMusic - 1) ? gMusic : 0, c.musicGen = gMusicGen;
    c.issued = t, c.seg = seg, c.pos = pos;
    if (!preview)
        c.src[0] = sourceOf(d.intro), c.src[2] = sourceOf(d.outro);
    c.src[1] = sourceOf(body);
    int stream = -1;
    if (body->streamed)
    {
        const uint64_t length = uint64_t(double(body->seconds) * body->rate);
        const uint64_t seek = seg == 1 && pos > 0 ? (length ? uint64_t(pos) % length : 0) : 0;
        if ((stream = takeStream(body, (c.flags & FlagLoop) != 0, seek)) < 0)
            return 0; // every stream busy
        c.src[1].stream = stream;
        if (seg == 1)
            c.pos = 0; // the decoder starts where it was
    }
    if (!push(c))
    {
        if (stream >= 0)
            gStreams[stream].state.store(StreamClose, std::memory_order_release); // topUp frees it
        return 0;
    }
    gDefVoices[id - 1].fetch_add(1, std::memory_order_relaxed);
    Mirror& m = gMirror[slot];
    m = Mirror{};
    m.used = true, m.gen = gen, m.def = id - 1, m.variation = variation, m.started = t, m.positional = at != nullptr, m.preview = preview;
    if (at)
        memcpy(m.at, at, sizeof(m.at));
    return (gen << 8) | slot;
}

uint32_t nextGen(uint32_t slot)
{
    const uint32_t g = (gMirror[slot].gen + 1) & 0xffffff;
    return g ? g : 1;
}

snd::Voice playDef(uint32_t sound, const snd::Play* p)
{
    if (!gLoaded || !gDevice || !sound || sound > kDefs)
        return 0;
    Def& d = gDefs[sound - 1];
    if (!d.used || d.retired || !d.variationCount)
        return 0;
    const double t = nowSeconds();
    if (t - d.lastPlay < d.o.cooldown)
        return 0;
    const bool   positional = p && (p->flags & 1);
    const float* at = positional ? p->at : nullptr;
    // Its instance limit: one gives way (or the play is skipped).
    uint32_t n = 0;
    int      oldest = -1, farthest = -1;
    float    farDist = -1;
    for (uint32_t i = 0; i < kVoices; ++i)
    {
        const Mirror& m = gMirror[i];
        if (!alive(i) || m.def != sound - 1 || m.stopping || m.preview)
            continue;
        ++n;
        if (oldest < 0 || m.started < gMirror[oldest].started)
            oldest = int(i);
        const float dist = m.positional ? distanceTo(m.at) : 0.0f;
        if (dist > farDist)
            farDist = dist, farthest = int(i);
    }
    if (n >= d.o.max)
    {
        uint32_t steal = d.o.steal;
        if (steal == StealAuto)
            steal = d.o.loop ? StealNone : positional ? StealFarthest : StealOldest;
        if (steal == StealNone)
            return 0;
        int victim = steal == StealOldest ? oldest : farthest;
        if (steal != StealOldest && farDist <= 0)
            victim = oldest; // none farther than another: the oldest
        if (steal != StealOldest && positional && farDist > 0 && distanceTo(at) > farDist)
            return 0; // the new one is the farthest of all: it gives way itself
        if (victim < 0)
            return 0;
        Command c = {};
        c.type = CmdStop, c.slot = uint32_t(victim), c.gen = gMirror[victim].gen, c.cut = CutFade, c.fade = 0.02f;
        if (!push(c))
            return 0;
        gMirror[victim].stopping = true;
    }
    const int slot = freeSlot();
    if (slot < 0)
        return 0;
    // Its variation: shuffled, never one of the last half played again (at least the last one).
    const uint32_t count = d.variationCount;
    uint32_t       pick = 0;
    if (count > 1)
    {
        const uint32_t avoid = std::min(std::max(1u, count / 2), std::min(d.recentCount, 8u));
        uint32_t       options[kVariations], no = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (std::find(d.recent, d.recent + avoid, i) == d.recent + avoid)
                options[no++] = i;
        pick = no ? options[std::min(no - 1, uint32_t(random01() * no))] : 0;
    }
    const float volume = (p ? p->volume : 0.0f) - random01() * d.o.jitterVolume;
    const float pitch = (p ? p->pitch : 0.0f) + (random01() * 2.0f - 1.0f) * d.o.jitterPitch;
    const uint32_t flags = (positional ? FlagAt : 0) | (p && (p->flags & 2) ? FlagSting : 0);
    const snd::Voice v = start(sound, uint32_t(slot), nextGen(uint32_t(slot)), pick, flags, at, volume, pitch, 0, 0, 0);
    if (!v)
        return 0;
    if (count > 1)
    {
        memmove(d.recent + 1, d.recent, sizeof(d.recent) - sizeof(d.recent[0]));
        d.recent[0] = pick, d.recentCount = std::min(8u, d.recentCount + 1);
    }
    d.lastPlay = t;
    return v;
}

// Resumes the voices kept for def `id` (its key came back).
void resume(uint32_t id)
{
    for (uint32_t i = 0; i < gPendingCount;)
    {
        Pending& p = gPending[i];
        if (p.key != gDefs[id - 1].key || p.variation >= gDefs[id - 1].variationCount || alive(p.slot))
        {
            ++i;
            continue;
        }
        const snd::Voice v = start(id, p.slot, p.gen, p.variation, p.flags & (FlagAt | FlagSting), (p.flags & FlagAt) ? p.at : nullptr, 0, 0,
                              p.seg, p.pos, 0.02f);
        if (v)
        {
            Command c = {};
            c.type = CmdTune, c.slot = p.slot, c.gen = p.gen, c.volume = p.tuneDb, c.pitch = p.tunePitch;
            push(c);
            if (p.flags & FlagMusic)
                gMusic = p.slot + 1, gMusicGen = p.gen, gMusicDef = id;
        }
        gPending[i] = gPending[--gPendingCount];
    }
}

void sendKnobs(uint32_t id)
{
    Command c = {};
    c.type = CmdKnobs, c.def = id - 1, c.volume = gDefs[id - 1].volumeDb, c.pitch = gDefs[id - 1].pitch;
    push(c);
}

// ---- The exports' calls (snd::Api) ----

void apiBegin(const char* owner)
{
    if (!gLoaded)
        return;
    for (Def& d : gDefs)
        if (d.used && !strcmp(d.owner, owner))
            d.confirmed = false;
}

uint32_t apiDefine(const snd::Spec* s, char* error, uint32_t errorBytes)
{
    if (!gLoaded || !s || !s->name || !s->owner)
        return snprintf(error, errorBytes, "the Sound component isn't running"), 0;
    if (strlen(s->name) >= 64 || (s->options && strlen(s->options) >= 256))
        return snprintf(error, errorBytes, "its name or options are too long"), 0;
    uint32_t variations = 0, intros = 0, outros = 0;
    uint64_t key = fnv(s->owner, strlen(s->owner));
    key = fnv(s->name, strlen(s->name) + 1, key);
    key = fnv(s->options ? s->options : "", s->options ? strlen(s->options) : 0, key);
    for (uint32_t i = 0; i < s->fileCount; ++i)
    {
        const snd::File& f = s->files[i];
        variations += f.kind == 0, intros += f.kind == 1, outros += f.kind == 2;
        const uint64_t h = fnv(f.bytes, f.size);
        key = fnv(&f.kind, sizeof(f.kind), key);
        key = fnv(&h, sizeof(h), key);
    }
    if (!variations)
        return snprintf(error, errorBytes, "no file"), 0;
    if (variations > kVariations || intros > 1 || outros > 1)
        return snprintf(error, errorBytes, "at most %u variations, one intro and one outro", kVariations), 0;
    // The same sound as before (an earlier version of its module's): the same id, its voices play on.
    int fresh = -1;
    for (uint32_t i = 0; i < kDefs; ++i)
    {
        Def& d = gDefs[i];
        if (d.used && !d.retired && d.key == key && !strcmp(d.owner, s->owner) && !strcmp(d.name, s->name))
        {
            d.confirmed = true;
            if (d.volumeDb != s->volume || d.pitch != s->pitch)
                d.volumeDb = s->volume, d.pitch = s->pitch, sendKnobs(i + 1);
            return i + 1;
        }
        if (!d.used && fresh < 0)
            fresh = int(i);
    }
    if (fresh < 0)
        return snprintf(error, errorBytes, "too many sounds at once (%u)", kDefs), 0;
    Opts o;
    if (!parse(s->options, intros || outros, &o, error, errorBytes))
        return 0;
    Clip* parts[kVariations + 2] = {};
    uint32_t np = 0;
    for (uint32_t i = 0; i < s->fileCount; ++i)
    {
        const snd::File& f = s->files[i];
        Clip*            c = clipFor(f.bytes, f.size, f.kind ? 2 : o.stream, o.music && f.kind == 0, error, errorBytes);
        if (!c)
            return 0;
        parts[np++] = c;
    }
    // A changed sound of the same name: its voices fade out and it goes once they have.
    for (uint32_t i = 0; i < kDefs; ++i)
        if (gDefs[i].used && !gDefs[i].retired && !strcmp(gDefs[i].owner, s->owner) && !strcmp(gDefs[i].name, s->name))
            gDefs[i].retired = true, gOrderDirty = true, stopVoices(i, 0.02f);
    Def& d = gDefs[fresh];
    memset(&d, 0, sizeof(d));
    copy(d.owner, s->owner, sizeof(d.owner)), copy(d.name, s->name, sizeof(d.name)), copy(d.options, s->options, sizeof(d.options));
    d.key = key, d.o = o, d.volumeDb = s->volume, d.pitch = s->pitch, d.lastPlay = -1e9, d.order = gOrderNext++;
    for (uint32_t i = 0; i < s->fileCount; ++i)
    {
        Clip* c = parts[i];
        c->refs++;
        if (s->files[i].kind == 1)
            d.intro = c;
        else if (s->files[i].kind == 2)
            d.outro = c;
        else
            d.variations[d.variationCount++] = c;
    }
    d.used = true, d.confirmed = true;
    gOrderDirty = true;
    gDefVoices[fresh] = 0;
    sendKnobs(fresh + 1);
    resume(fresh + 1);
    return fresh + 1;
}

void apiEnd(const char* owner)
{
    if (!gLoaded)
        return;
    for (uint32_t i = 0; i < kDefs; ++i)
    {
        Def& d = gDefs[i];
        if (d.used && !d.retired && !d.confirmed && !strcmp(d.owner, owner))
            d.retired = true, gOrderDirty = true, stopVoices(i, 0.02f);
    }
}

void apiKnobs(uint32_t sound, float volumeDb, float pitch)
{
    if (!gLoaded || !sound || sound > kDefs || !gDefs[sound - 1].used)
        return;
    gDefs[sound - 1].volumeDb = volumeDb, gDefs[sound - 1].pitch = pitch;
    sendKnobs(sound);
}

void apiStop(snd::Voice v, uint32_t cut, float seconds)
{
    uint32_t slot;
    if (!gLoaded || !valid(v, &slot) || gMirror[slot].stopping)
        return;
    Command c = {};
    c.type = CmdStop, c.slot = slot, c.gen = gMirror[slot].gen, c.cut = uint8_t(cut <= CutRelease ? cut : CutDefault);
    c.fade = std::max(0.0f, seconds);
    if (push(c)) // playing() stays true through its fade, outro or the rest of its pass; it no longer counts toward its limit
        gMirror[slot].stopping = true;
}

void apiMove(snd::Voice v, const float* at)
{
    uint32_t slot;
    if (!gLoaded || !at || !valid(v, &slot))
        return;
    Command c = {};
    c.type = CmdMove, c.slot = slot, c.gen = gMirror[slot].gen;
    memcpy(c.a, at, sizeof(c.a));
    if (push(c))
        memcpy(gMirror[slot].at, at, sizeof(c.a));
}

void apiTune(snd::Voice v, float volumeDb, float pitch)
{
    uint32_t slot;
    if (!gLoaded || !valid(v, &slot))
        return;
    Command c = {};
    c.type = CmdTune, c.slot = slot, c.gen = gMirror[slot].gen, c.volume = volumeDb, c.pitch = pitch;
    push(c);
}

int apiPlaying(snd::Voice v)
{
    uint32_t slot;
    return gLoaded && valid(v, &slot) ? 1 : 0;
}

void apiMusic(uint32_t sound)
{
    if (!gLoaded)
        return;
    const bool on = gMusic && alive(gMusic - 1) && gMirror[gMusic - 1].gen == gMusicGen && !gMirror[gMusic - 1].stopping;
    if (sound && on && gMusicDef == sound)
        return; // the same music: nothing changes
    if (!sound)
    {
        if (on)
        {
            Command c = {};
            c.type = CmdMusicOff, c.fade = 2.0f;
            if (push(c))
                gMirror[gMusic - 1].stopping = true;
        }
        gMusic = gMusicGen = gMusicDef = 0;
        return;
    }
    if (sound > kDefs || !gDefs[sound - 1].used || !gDefs[sound - 1].o.music)
        return;
    const snd::Voice v = playDef(sound, nullptr);
    if (!v)
        return;
    if (on)
        gMirror[gMusic - 1].stopping = true;
    gMusic = (v & 0xff) + 1, gMusicGen = v >> 8, gMusicDef = sound;
}

void apiListener(const float* at, const float* forward, const float* up)
{
    if (!gLoaded || !at || !forward || !up)
        return;
    Command c = {};
    c.type = CmdListener;
    memcpy(c.a, at, sizeof(c.a)), memcpy(c.b, forward, sizeof(c.b)), memcpy(c.c, up, sizeof(c.c));
    if (push(c))
        memcpy(gListener, at, sizeof(gListener));
}

float gBusGame[BusCount] = {}; // a game's own offsets (its options menu)
float gBusSent[BusCount] = {}, gMasterSent = -1;

void apiBus(uint32_t b, float volumeDb)
{
    if (gLoaded && b < BusCount)
        gBusGame[b] = std::max(-96.0f, std::min(24.0f, volumeDb));
}

void sortOrder()
{
    if (!gOrderDirty)
        return;
    gOrderDirty = false, gOrderCount = 0;
    for (uint32_t i = 0; i < kDefs; ++i)
        if (gDefs[i].used && !gDefs[i].retired)
            gOrder[gOrderCount++] = i;
    std::sort(gOrder, gOrder + gOrderCount, [](uint32_t a, uint32_t b) { return gDefs[a].order < gDefs[b].order; });
}

uint32_t apiCount()
{
    if (!gLoaded)
        return 0;
    sortOrder();
    return gOrderCount;
}

int apiInfo(uint32_t index, snd::Info* out)
{
    sortOrder();
    if (!gLoaded || !out || index >= gOrderCount)
        return 0;
    const Def& d = gDefs[gOrder[index]];
    out->name = d.name, out->owner = d.owner, out->options = d.options;
    out->id = gOrder[index] + 1, out->bus = d.o.bus;
    out->flags = (d.o.loop ? 1 : 0) | (d.o.music ? 2 : 0) | (d.variations[0]->streamed ? 4 : 0);
    out->files = d.variationCount + (d.intro ? 1 : 0) + (d.outro ? 1 : 0);
    out->voices = uint32_t(std::max(0, gDefVoices[gOrder[index]].load(std::memory_order_relaxed)));
    return 1;
}

// A sound's files in order: its variations, then its intro, then its outro.
const Clip* fileOf(const Def& d, uint32_t file, uint32_t* kind)
{
    if (file < d.variationCount)
        return *kind = 0, d.variations[file];
    file -= d.variationCount;
    if (d.intro && file == 0)
        return *kind = 1, d.intro;
    file -= d.intro ? 1 : 0;
    if (d.outro && file == 0)
        return *kind = 2, d.outro;
    return nullptr;
}

int apiWave(uint32_t sound, uint32_t file, snd::Wave* out)
{
    if (!gLoaded || !out || !sound || sound > kDefs || !gDefs[sound - 1].used)
        return 0;
    uint32_t    kind = 0;
    const Clip* c = fileOf(gDefs[sound - 1], file, &kind);
    if (!c)
        return 0;
    out->kind = kind, out->channels = c->channels, out->rate = c->rate, out->peaksReady = c->peaksReady;
    out->seconds = c->seconds, out->peaks = c->peaks;
    return 1;
}

snd::Voice apiPreview(uint32_t sound, uint32_t file)
{
    if (!gLoaded || !gDevice || !sound || sound > kDefs || !gDefs[sound - 1].used)
        return 0;
    apiStop(gPreview, CutFade, 0.02f);
    Def&        d = gDefs[sound - 1];
    uint32_t    kind = 0;
    const Clip* c = fileOf(d, file, &kind);
    const int   slot = freeSlot();
    if (!c || slot < 0)
        return 0;
    // Played as a one-shot of that file alone: a variation in place, an intro or outro as the body.
    Clip* const keep = d.variations[0];
    d.variations[0] = (Clip*)c;
    const snd::Voice v = start(sound, uint32_t(slot), nextGen(uint32_t(slot)), kind == 0 ? file : 0, FlagPreview, nullptr, 0, 0, 1, 0, 0);
    d.variations[0] = keep;
    return gPreview = v;
}

float apiPosition(snd::Voice v)
{
    uint32_t slot;
    return gLoaded && valid(v, &slot) ? gPosition[slot].load(std::memory_order_relaxed) : 0.0f;
}

snd::Api gApi = { snd::kApi,   sizeof(snd::Api), 0,          apiBegin,    apiDefine, apiEnd,   apiKnobs,   playDef,
                  apiStop,     apiMove,          apiTune,    apiPlaying,  apiMusic,  apiListener, apiBus, apiCount,
                  apiInfo,     apiWave,          apiPreview, apiPosition };

// ---- The device ----

ma_context gContext;
ma_device  gDev;
bool       gContextOpen = false;
uint64_t   gCallbacksSeen = 0;
double     gCallbacksMoved = 0;

void callback(ma_device*, void* out, const void*, ma_uint32 count) { render((float*)out, count); }

bool openDevice(bool fresh)
{
    ma_device_config c = ma_device_config_init(ma_device_type_playback);
    c.playback.format = ma_format_f32;
    c.playback.channels = 2;
    c.sampleRate = 0; // the device's own
    c.dataCallback = callback;
    c.periodSizeInMilliseconds = 10;
    c.performanceProfile = ma_performance_profile_low_latency;
    c.noPreSilencedOutputBuffer = MA_TRUE; // render() writes every frame
    c.noClip = MA_TRUE;                    // the limiter does
#if defined(_WIN32)
    sessionBegin();
#endif
    if (ma_device_init(&gContext, &c, &gDev) != MA_SUCCESS)
        return false;
#if defined(MA_HAS_WASAPI)
    if (gContext.backend == ma_backend_wasapi)
        sessionName(gDev.wasapi.pAudioClientPlayback);
#endif
    if (fresh)
        reset(gDev.sampleRate);
    else
        setRate(gDev.sampleRate);
    if (ma_device_start(&gDev) != MA_SUCCESS)
    {
        ma_device_uninit(&gDev);
        return false;
    }
    gCallbacksSeen = gCallbacks.load(), gCallbacksMoved = nowSeconds();
    return true;
}

// ---- Frame-by-frame ----

uint64_t gUpdated = ~0ull; // the frame update() last ran in: draw() without it is F1's Pause
bool     gPaused = false;

void collect()
{
    for (uint32_t i = 0; i < kStreams; ++i)
        topUp(i); // closes the streams of voices that ended, before their files go
    for (uint32_t i = 0; i < kDefs; ++i)
        if (gDefs[i].used && gDefs[i].retired && gDefVoices[i].load(std::memory_order_acquire) <= 0)
            releaseDef(i);
    for (uint32_t i = 0; i < kClips; ++i)
    {
        Clip* c = gClips[i];
        if (!c || c->refs > 0)
            continue;
        bool busy = gScan.clip == c;
        for (const Decoding& d : gDecoding)
            busy |= d.clip == c;
        if (!busy)
            freeClip(i);
    }
    if (gPendingCount && nowSeconds() > gPendingUntil)
        gPendingCount = 0;
}

void sendMix()
{
    const float knobs[BusCount] = { settings.effects, settings.interface, settings.voice, settings.ambience, settings.music };
    for (uint32_t b = 0; b < BusCount; ++b)
    {
        const float db = knobs[b] + gBusGame[b];
        if (db == gBusSent[b])
            continue;
        Command c = {};
        c.type = CmdBus, c.bus = uint8_t(b), c.volume = db;
        if (push(c))
            gBusSent[b] = db;
    }
    if (settings.volume != gMasterSent)
    {
        Command c = {};
        c.type = CmdMaster, c.volume = settings.volume <= -40.0f ? 0.0f : powf(10.0f, settings.volume / 20.0f);
        if (push(c))
            gMasterSent = settings.volume;
    }
}

void clearAll()
{
    for (uint32_t i = 0; i < kStreams; ++i)
    {
        if (gDecoding[i].open)
            ma_decoder_uninit(&gDecoding[i].decoder);
        gDecoding[i] = Decoding{};
    }
    stopScan();
    for (uint32_t i = 0; i < kClips; ++i)
        if (gClips[i])
            freeClip(i);
    memset(gDefs, 0, sizeof(gDefs));
    memset(gMirror, 0, sizeof(gMirror));
    memset(gListener, 0, sizeof(gListener));
    for (float& b : gBusGame)
        b = 0;
    for (float& b : gBusSent)
        b = 0;
    gMasterSent = -1;
    gMusic = gMusicGen = gMusicDef = 0, gPreview = 0;
    gOrderCount = 0, gOrderDirty = true, gPendingCount = 0;
    gUpdated = ~0ull, gPaused = false;
}
} // namespace

uint64_t api(uint32_t version)
{
    if (!gLoaded)
        return 0;
    if (version == 0)
        return 1; // running, whatever its version
    return version == snd::kApi ? (uint64_t)(uintptr_t)&gApi : 0;
}

int load(int)
{
    clearAll();
    reset(48000);
    // Loops and music the version before kept: resumed once their sounds are registered again.
    const double now = yea::host()->time();
    for (const Voice& r : Voices)
        if (r.stamp == stamp() && r.frame <= yea::host()->frame() && r.time <= now && gPendingCount < kVoices && (r.handle & 0xff) < kVoices)
        {
            Pending& p = gPending[gPendingCount++];
            p.key = r.key, p.slot = r.handle & 0xff, p.gen = r.handle >> 8, p.seg = r.seg, p.variation = r.variation, p.flags = r.flags;
            p.pos = r.pos, p.at[0] = r.at.x, p.at[1] = r.at.y, p.at[2] = r.at.z, p.tuneDb = r.tune_db, p.tunePitch = r.tune_pitch;
            gMirror[p.slot].gen = p.gen; // its handle stays the game's
        }
    Voices.clear();
    gPendingUntil = nowSeconds() + 5.0;
    gApi.instance = (uint64_t)(nowSeconds() * 1e6) ^ (stamp() << 1);
    // The device last: once its thread runs this module's code, nothing here may fail.
    gContextOpen = ma_context_init(nullptr, 0, nullptr, &gContext) == MA_SUCCESS;
    gDevice = gContextOpen && openDevice(true);
    if (!gDevice)
    {
        // No sound device (none plugged in, a headless machine): a silent one keeps time, so sounds
        // still start and end as they would.
        const ma_backend silent = ma_backend_null;
        if (gContextOpen)
            ma_context_uninit(&gContext);
        gContextOpen = ma_context_init(&silent, 1, nullptr, &gContext) == MA_SUCCESS;
        gDevice = gContextOpen && openDevice(true);
    }
    char line[96];
    snprintf(line, sizeof(line), "sound: %s at %u Hz", gDevice ? ma_get_backend_name(gContext.backend) : "no device", gDevice ? gDev.sampleRate : 0);
    yea::host()->log(0, line);
    gLoaded = true;
    return 0;
}

void unload()
{
    gLoaded = false;
    if (gDevice)
    {
        // Every voice fades out over 15 ms (no click), then the device stops and its thread is gone.
        Command c = {};
        c.type = CmdLeave, c.fade = 0.015f;
        if (push(c))
        {
            const uint64_t from = gCallbacks.load();
            for (int i = 0; i < 8 && gCallbacks.load() < from + 3; ++i)
                nap(5);
        }
        ma_device_uninit(&gDev);
        gDevice = false;
        // Loops and music still playing are kept for the next version (a swap of this module's code).
        Voices.clear();
        for (uint32_t i = 0; i < kVoices; ++i)
        {
            State s;
            if (!state(i, &s) || s.stopping || s.preview || !(s.flags & (FlagLoop | FlagMusic)) || s.def >= kDefs || !gDefs[s.def].used ||
                gDefs[s.def].retired || gMirror[i].gen != s.gen)
                continue;
            Voice* r = Voices.add();
            if (!r)
                break;
            r->key = gDefs[s.def].key, r->handle = (s.gen << 8) | i, r->seg = s.seg, r->variation = gMirror[i].variation;
            r->flags = (s.flags & (FlagAt | FlagMusic)) | (gMusic == i + 1 ? FlagMusic : 0);
            r->pos = s.pos, r->at = yea_float3{ s.at[0], s.at[1], s.at[2] }, r->tune_db = s.tuneDb, r->tune_pitch = s.tunePitch;
            if (gDecoding[0].clip || gDecoding[1].clip || gDecoding[2].clip || gDecoding[3].clip)
            {
                // A streamed body: where its decoder has read to, less what the ring holds unplayed.
                for (uint32_t k = 0; k < kStreams; ++k)
                    if (gDecoding[k].clip == gDefs[s.def].variations[r->variation] && s.seg == 1)
                        r->pos = double(gStreams[k].read.load()) + double(gDecoding[k].seek);
            }
            r->stamp = stamp(), r->frame = yea::host()->frame(), r->time = yea::host()->time();
        }
    }
    if (gContextOpen)
        ma_context_uninit(&gContext), gContextOpen = false;
    clearAll();
}

void update(uint32_t phase, float)
{
    if (phase != YEA_PHASE_INPUT || !gLoaded)
        return;
    gUpdated = yea::host()->frame();
    sendMix();
    collect();
}

void draw(uint32_t pass, const YeaFrame*)
{
    if (pass != YEA_PASS_WORLD || !gLoaded)
        return;
    // F1's Pause stops update(), not draw(): every voice holds where it is, a preview plays on.
    const bool paused = gUpdated != yea::host()->frame();
    if (paused != gPaused)
    {
        Command c = {};
        c.type = CmdPause, c.volume = paused ? 0.0f : 1.0f;
        if (push(c))
            gPaused = paused;
    }
    for (uint32_t i = 0; i < kStreams; ++i)
        topUp(i);
    scanPeaks();
    // A device that stopped calling back (lost, asleep, rerouted) is opened again.
    if (gDevice)
    {
        const uint64_t calls = gCallbacks.load(std::memory_order_relaxed);
        const double   t = nowSeconds();
        if (calls != gCallbacksSeen)
            gCallbacksSeen = calls, gCallbacksMoved = t;
        else if (t - gCallbacksMoved > 2.0)
        {
            ma_device_uninit(&gDev);
            gDevice = openDevice(false);
            gCallbacksMoved = t;
        }
    }
}
} // namespace sound
