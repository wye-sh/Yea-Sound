// The mixer (Mixer.h): voices, buses, ducking and the limiter, on the sound device's thread. Kept small on
// purpose: Yea's crash guard does not cover this thread, so it touches nothing but its own state, the
// command ring and buffers the main thread keeps alive for it, with every index checked.
#include "Mixer.h"

#include <algorithm>
#include <chrono>
#include <math.h>
#include <string.h>

namespace sound
{
namespace mix
{
Stream                gStreams[kStreams];
std::atomic<uint32_t> gDoneGen[kVoices];
std::atomic<int>      gDefVoices[kDefs];
std::atomic<uint64_t> gCallbacks{ 0 };
std::atomic<float>    gPosition[kVoices];

namespace
{
constexpr uint32_t kChunk = 64;      // frames mixed between gain updates (ramps are linear across it)
constexpr float    kSilent = 0.001f; // -60 dB: below it a one-shot ends and a loop goes virtual
constexpr float    kDuckDb = -8.0f, kDuckIn = 0.2f, kDuckOut = 1.0f;
constexpr float    kCeiling = 0.891f; // the limiter's (-1 dBFS)
constexpr float    kPi = 3.14159265f;

double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
float  dbToGain(float db) { return db <= -96.0f ? 0.0f : powf(10.0f, db / 20.0f); }

Command               gRing[kCommands];
std::atomic<uint32_t> gHead{ 0 }, gTail{ 0 }; // main writes head, mixer tail

struct Env
{
    float from = 0, target = 0, t = 1, dt = 0; // t: 0..1 through a fade from `from` to `target`
    float value() const
    {
        if (t >= 1.0f)
            return target;
        // Equal power: up on a sine, down on a cosine, as a crossfade wants.
        return target > from ? from + (target - from) * sinf(t * kPi * 0.5f) : target + (from - target) * cosf(t * kPi * 0.5f);
    }
    void go(float to, float seconds, float rate)
    {
        from = value(), target = to, t = 0, dt = 1.0f / std::max(seconds, kRamp) / rate;
    }
};

struct Voice
{
    bool     active = false;
    uint32_t gen = 0, def = 0, bus = 0, priority = 1, flags = 0, cut = 0;
    Source   src[3];
    uint32_t seg = 1;      // playing src[seg]
    double   pos = 0;      // in that segment's frames
    uint64_t streamAt = 0; // a stream's absolute frame at pos 0 of this pass
    double   musicSecs = 0;
    float    volume = 0, pitch = 0, tuneDb = 0, tunePitch = 0;
    float    at[3] = {}, nearDist = 2, farDist = 40;
    float    bpm = 120;
    uint32_t beats = 4;
    Env      env;
    bool     stopping = false;  // ends once env reaches 0
    bool     leaving = false;   // CmdLeave: fading out for an unload, not stopped by the game
    bool     releasing = false; // ends (or goes to the outro) at its loop pass's end
    float    cutFade = 0.15f;
    uint64_t delay = 0;         // frames before it starts
    uint64_t stopIn = ~0ull;    // frames till a stop (a music switch's fade) begins
    float    stopFade = 0;
    uint32_t after = 0, afterGen = 0; // starts once that voice (slot+1) has ended
    float    gl = 0, gr = 0;          // last chunk's gains
    bool     audible = false;
    // What a cut into the outro left playing under it, gone over 10 ms so the switch never clicks.
    Source   tail;
    double   tailPos = 0;
    uint64_t tailAt = 0;
    bool     tailLoop = false;
    float    tailGain = 0;
};

struct Mixer
{
    Voice    voices[kVoices];
    float    defVolume[kDefs] = {}, defPitch[kDefs] = {};
    float    busDb[BusCount] = {};
    float    duckDb = 0;
    float    listener[3] = {}, right[3] = { 1, 0, 0 };
    float    master = 1, pause = 1, pauseTarget = 1;
    float    limiter = 1;
    uint32_t rate = 48000;
    uint32_t music = 0; // slot+1 of the view's music
} gMix;

bool     has(const Source& s) { return s.buffer || (s.stream >= 0 && s.stream < int(kStreams)); }
uint32_t rateOf(const Source& s) { return s.buffer ? s.buffer->rate : gStreams[s.stream].rate; }
uint32_t channelsOf(const Source& s) { return s.buffer ? s.buffer->channels : gStreams[s.stream].channels; }

float distanceGain(const Voice& v, float* pan)
{
    const float dx = v.at[0] - gMix.listener[0], dy = v.at[1] - gMix.listener[1], dz = v.at[2] - gMix.listener[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    const float n = std::max(v.nearDist, 0.001f), f = std::max(v.farDist, n + 0.001f);
    // Inverse tapered: inverse rolloff from near, and quieter still toward far, where it is silent.
    const float inv = d <= n ? 1.0f : n / d;
    const float lin = d >= f ? 0.0f : d <= n ? 1.0f : 1.0f - (d - n) / (f - n);
    const float side = d > 1e-4f ? (dx * gMix.right[0] + dy * gMix.right[1] + dz * gMix.right[2]) / d : 0.0f;
    *pan = std::max(-1.0f, std::min(1.0f, side * std::min(1.0f, d / n))); // centred within near
    return std::min(inv, lin * lin);
}

void endVoice(uint32_t i)
{
    Voice& v = gMix.voices[i];
    for (Source& s : v.src)
        if (s.stream >= 0 && s.stream < int(kStreams))
        {
            gStreams[s.stream].state.store(StreamClose, std::memory_order_release);
            s.stream = -1;
        }
    v.active = false;
    if (gMix.music == i + 1)
        gMix.music = 0;
    if (v.def < kDefs)
        gDefVoices[v.def].fetch_sub(1, std::memory_order_relaxed);
    gDoneGen[i].store(v.gen, std::memory_order_release);
}

void beginStop(Voice& v, uint32_t cut, float seconds)
{
    const float rate = (float)gMix.rate;
    if (cut == CutDefault)
        cut = v.cut, seconds = v.cutFade;
    if (v.delay) // never started: nothing to hear
    {
        v.stopping = true, v.env = Env{}, v.delay = 0;
        return;
    }
    if (cut == CutRelease && (v.flags & FlagLoop) && v.seg <= 1)
    {
        v.releasing = true;
        return;
    }
    if ((cut == CutOutro || cut == CutRelease) && has(v.src[2]) && v.seg <= 1)
    {
        // The outro takes over at once, at the level the loop had, the loop under it gone in 10 ms.
        v.tail = v.src[v.seg], v.tailPos = v.pos, v.tailAt = v.streamAt, v.tailGain = 1.0f;
        v.tailLoop = v.seg == 1 && (v.flags & FlagLoop);
        v.seg = 2, v.pos = 0, v.streamAt = 0;
        v.flags &= ~uint32_t(FlagLoop);
        return;
    }
    v.stopping = true;
    v.env.go(0.0f, cut == CutFade ? seconds : kRamp, rate);
}

// Musical time of the view's music: the frames till its next beat or bar (0: no music).
uint64_t untilNext(uint32_t music, uint32_t musicGen, bool bar)
{
    if (!music || music > kVoices)
        return 0;
    const Voice& m = gMix.voices[music - 1];
    if (!m.active || m.gen != musicGen || m.stopping)
        return 0;
    const double beat = 60.0 / std::max(1.0f, m.bpm);
    const double unit = bar ? beat * std::max(1u, m.beats) : beat;
    const double next = ceil(m.musicSecs / unit - 1e-6) * unit;
    return uint64_t(std::max(0.0, next - m.musicSecs) * gMix.rate + m.delay);
}

void run(const Command& c)
{
    Mixer&      M = gMix;
    const float rate = (float)M.rate;
    if (c.type == CmdPlay)
    {
        if (c.slot >= kVoices || c.def >= kDefs)
            return;
        Voice& v = M.voices[c.slot];
        if (v.active) // the slot's last voice never ended (main waits for it, so it can't happen)
            endVoice(c.slot);
        const bool loop = c.flags & FlagLoop;
        v = Voice{};
        v.active = true, v.gen = c.gen, v.def = c.def, v.src[0] = c.src[0], v.src[1] = c.src[1], v.src[2] = c.src[2];
        // A one-shot queued while there was no device is not played late.
        if (!loop && nowSeconds() - c.issued > 0.25)
        {
            endVoice(c.slot);
            return;
        }
        v.bus = c.bus < BusCount ? c.bus : 0, v.priority = c.priority, v.flags = c.flags, v.cut = c.cut;
        v.seg = c.seg <= 2 && has(c.src[c.seg]) ? c.seg : has(c.src[0]) ? 0 : 1;
        v.pos = c.seg == v.seg && c.src[v.seg].buffer ? std::min(c.pos, double(c.src[v.seg].buffer->count)) : 0;
        v.musicSecs = c.pos / std::max(1u, has(c.src[v.seg]) ? rateOf(c.src[v.seg]) : 1u);
        v.volume = c.volume, v.pitch = c.pitch, v.cutFade = c.fade;
        memcpy(v.at, c.a, sizeof(v.at));
        v.nearDist = c.nearDist, v.farDist = c.farDist, v.bpm = c.bpm, v.beats = c.beats;
        float fadeIn = c.fadeIn;
        if (c.flags & FlagMusic)
        {
            // The view's music: the one playing hands over on its beat, bar or loop's end, crossfading.
            const uint32_t old = M.music;
            if (old && old != c.slot + 1 && M.voices[old - 1].active && !M.voices[old - 1].stopping)
            {
                Voice& o = M.voices[old - 1];
                if (c.switchAt == SwitchEnd)
                {
                    o.releasing = true, o.cut = CutRelease;
                    v.after = old, v.afterGen = o.gen;
                }
                else
                {
                    const uint64_t wait = c.switchAt == SwitchNow ? 0 : untilNext(old, o.gen, c.switchAt == SwitchBar);
                    v.delay = wait, o.stopIn = wait, o.stopFade = c.crossfade;
                    fadeIn = std::max(fadeIn, c.crossfade);
                }
            }
            M.music = c.slot + 1;
        }
        else if (c.flags & FlagSting)
            v.delay = untilNext(c.music, c.musicGen, false);
        v.env.target = 0, v.env.t = 1;
        v.env.go(1.0f, fadeIn, rate);
        return;
    }
    if (c.type == CmdKnobs)
    {
        if (c.def < kDefs)
            M.defVolume[c.def] = c.volume, M.defPitch[c.def] = c.pitch;
        return;
    }
    if (c.type == CmdBus)
    {
        if (c.bus < BusCount)
            M.busDb[c.bus] = c.volume;
        return;
    }
    if (c.type == CmdListener)
    {
        memcpy(M.listener, c.a, sizeof(M.listener));
        // right = forward x up
        const float* f = c.b;
        const float* u = c.c;
        float        r[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
        const float  l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (l > 1e-6f)
            M.right[0] = r[0] / l, M.right[1] = r[1] / l, M.right[2] = r[2] / l;
        return;
    }
    if (c.type == CmdPause)
    {
        M.pauseTarget = c.volume;
        return;
    }
    if (c.type == CmdMaster)
    {
        M.master = c.volume;
        return;
    }
    if (c.type == CmdMusicOff)
    {
        if (M.music && M.voices[M.music - 1].active)
            beginStop(M.voices[M.music - 1], CutFade, c.fade);
        M.music = 0;
        return;
    }
    if (c.type == CmdLeave)
    {
        for (Voice& v : M.voices)
            if (v.active)
                v.leaving = true, v.delay = 0, v.env.go(0.0f, c.fade, rate);
        M.pause = M.pauseTarget = 1; // heard fading even while paused
        return;
    }
    if (c.slot >= kVoices)
        return;
    Voice& v = M.voices[c.slot];
    if (!v.active || v.gen != c.gen)
        return;
    if (c.type == CmdStop)
    {
        if (!v.stopping)
            beginStop(v, c.cut, c.fade);
        else if (c.cut == CutNow || (c.cut == CutFade && c.fade < (1.0f - v.env.t) / std::max(v.env.dt, 1e-9f) / rate))
            v.env.go(0.0f, c.cut == CutNow ? kRamp : c.fade, rate); // a quicker stop than the one under way
    }
    else if (c.type == CmdMove)
        memcpy(v.at, c.a, sizeof(v.at));
    else if (c.type == CmdTune)
        v.tuneDb = c.volume, v.tunePitch = c.pitch;
}

// One frame of src at fractional position p (interpolated; at a loop's end, the next frame is its start).
inline void sample(const Source& s, double p, bool wrap, uint64_t streamAt, float* l, float* r)
{
    const uint64_t i = (uint64_t)p;
    const float    t = float(p - double(i));
    if (s.buffer)
    {
        const Buffer&  b = *s.buffer;
        const uint64_t a0 = i < b.count ? i : b.count - 1;
        const uint64_t a1 = a0 + 1 < b.count ? a0 + 1 : (wrap ? 0 : a0);
        const float*   a = &b.frames[a0 * b.channels];
        const float*   c = &b.frames[a1 * b.channels];
        if (b.channels == 1)
            *l = *r = a[0] + (c[0] - a[0]) * t;
        else
            *l = a[0] + (c[0] - a[0]) * t, *r = a[1] + (c[1] - a[1]) * t;
        return;
    }
    const Stream&  st = gStreams[s.stream];
    const uint64_t at = streamAt + i, w = st.written.load(std::memory_order_acquire);
    if (at >= w)
    {
        *l = *r = 0;
        return;
    }
    const uint64_t next = at + 1 < w ? at + 1 : at;
    const float*   a = &st.frames[(at % kRing) * st.channels];
    const float*   c = &st.frames[(next % kRing) * st.channels];
    if (st.channels == 1)
        *l = *r = a[0] + (c[0] - a[0]) * t;
    else
        *l = a[0] + (c[0] - a[0]) * t, *r = a[1] + (c[1] - a[1]) * t;
}

// The next loop pass's start in a stream after absolute frame `from` (~0: none known yet).
uint64_t nextWrap(const Stream& st, uint64_t from)
{
    uint64_t best = ~0ull;
    for (const auto& w : st.wraps)
    {
        const uint64_t x = w.load(std::memory_order_acquire);
        if (x > from && x < best)
            best = x;
    }
    return best;
}

// Moves voice v on by `n` output frames, mixing into out (stereo) with gains going from (gl0, gr0) to
// (gl1, gr1) across them when out is set. Returns false once it has ended.
bool advance(Voice& v, uint32_t n, float* out, float gl0, float gr0, float gl1, float gr1, float pitchRatio)
{
    const float rate = (float)gMix.rate;
    for (uint32_t k = 0; k < n; ++k)
    {
        Source&      s = v.src[v.seg];
        const bool   loopSeg = v.seg == 1 && (v.flags & FlagLoop);
        const double step = double(rateOf(s)) / rate * pitchRatio;
        // Where this segment ends: a buffer's length, a stream's end, or its next loop pass when releasing.
        uint64_t end;
        if (s.buffer)
            end = s.buffer->count;
        else
        {
            Stream& st = gStreams[s.stream];
            if (st.state.load(std::memory_order_acquire) != StreamReady)
                return true; // not decoded yet: waits, silent, where it is
            const uint64_t e = st.ended.load(std::memory_order_acquire);
            end = e != ~0ull ? e - v.streamAt : ~0ull;
            if (loopSeg && v.releasing)
            {
                const uint64_t w = nextWrap(st, v.streamAt + (uint64_t)v.pos);
                if (w != ~0ull)
                    end = w - v.streamAt;
            }
        }
        if (v.pos >= double(end))
        {
            if (loopSeg && !v.releasing && s.buffer)
                v.pos -= double(end);
            else if (v.seg == 0 || (v.seg == 1 && has(v.src[2]) && (v.releasing || !(v.flags & FlagLoop))))
            {
                // On to the next part: the loop after the intro, the outro after a released loop.
                if (!s.buffer)
                    gStreams[s.stream].read.store(v.streamAt + end, std::memory_order_release);
                v.seg = v.seg == 0 ? 1 : 2, v.pos = 0, v.streamAt = 0;
                if (v.seg == 1 && !v.src[1].buffer && has(v.src[1]))
                    v.streamAt = gStreams[v.src[1].stream].read.load(std::memory_order_acquire);
                if (!has(v.src[v.seg]))
                    return false;
                --k;
                continue;
            }
            else
                return false;
        }
        if (out)
        {
            float l, r;
            sample(s, v.pos, loopSeg && !v.releasing && s.buffer != nullptr, v.streamAt, &l, &r);
            if (v.tailGain > 0.0f)
            {
                float          tl, tr;
                const uint32_t len = v.tail.buffer ? v.tail.buffer->count : ~0u;
                if (v.tailPos < double(len))
                {
                    sample(v.tail, v.tailPos, v.tailLoop && v.tail.buffer, v.tailAt, &tl, &tr);
                    l += tl * v.tailGain, r += tr * v.tailGain;
                }
                v.tailPos += double(rateOf(v.tail)) / rate * pitchRatio;
                if (v.tailLoop && v.tailPos >= double(len))
                    v.tailPos -= double(len);
                v.tailGain = std::max(0.0f, v.tailGain - 1.0f / (0.01f * rate));
            }
            const float t = (k + 0.5f) / n;
            out[2 * k] += l * (gl0 + (gl1 - gl0) * t);
            out[2 * k + 1] += r * (gr0 + (gr1 - gr0) * t);
        }
        else
            v.tailGain = 0;
        v.pos += step;
        v.musicSecs += pitchRatio / rate;
        if (!s.buffer) // a stream: what is behind us may be written over
        {
            Stream&        st = gStreams[s.stream];
            const uint64_t at = v.streamAt + (uint64_t)v.pos;
            if (at > st.read.load(std::memory_order_relaxed))
                st.read.store(std::min(at, st.written.load(std::memory_order_acquire)), std::memory_order_release);
        }
        if (v.env.t < 1.0f)
            v.env.t = std::min(1.0f, v.env.t + v.env.dt);
    }
    return !(v.stopping && v.env.t >= 1.0f && v.env.target <= 0.0f);
}
} // namespace

bool push(const Command& c)
{
    const uint32_t h = gHead.load(std::memory_order_relaxed);
    if (h - gTail.load(std::memory_order_acquire) >= kCommands)
        return false;
    gRing[h % kCommands] = c;
    gHead.store(h + 1, std::memory_order_release);
    return true;
}

void reset(uint32_t rate)
{
    gMix = Mixer{};
    gMix.rate = rate ? rate : 48000;
    gHead = 0, gTail = 0, gCallbacks = 0;
    for (auto& d : gDoneGen)
        d = 0;
    for (auto& d : gDefVoices)
        d = 0;
    for (auto& p : gPosition)
        p = 0;
    for (Stream& st : gStreams)
    {
        st.state = StreamFree, st.written = 0, st.read = 0, st.ended = ~0ull, st.wrapCount = 0;
        for (auto& w : st.wraps)
            w = 0;
    }
}

void setRate(uint32_t rate) { gMix.rate = rate ? rate : gMix.rate; }

bool state(uint32_t slot, State* out)
{
    if (slot >= kVoices)
        return false;
    const Voice& v = gMix.voices[slot];
    *out = State{};
    out->active = v.active, out->stopping = v.stopping || v.releasing, out->preview = v.flags & FlagPreview;
    out->gen = v.gen, out->def = v.def, out->seg = v.seg, out->flags = v.flags;
    out->pos = v.pos, out->musicSecs = v.musicSecs;
    memcpy(out->at, v.at, sizeof(out->at));
    out->tuneDb = v.tuneDb, out->tunePitch = v.tunePitch;
    return v.active;
}

void render(float* out, uint32_t count)
{
    Mixer& M = gMix;
    memset(out, 0, sizeof(float) * 2 * count);
    const uint32_t head = gHead.load(std::memory_order_acquire);
    uint32_t       tail = gTail.load(std::memory_order_relaxed);
    for (; tail != head; ++tail)
        run(gRing[tail % kCommands]);
    gTail.store(tail, std::memory_order_release);
    const float rate = (float)M.rate;

    // Who is heard this callback: the kMixed loudest by priority, the rest virtual (loops) or ended.
    struct Pick
    {
        float    score;
        uint32_t i;
    };
    Pick     picks[kVoices];
    uint32_t np = 0;
    bool     voiceBus = false;
    for (uint32_t i = 0; i < kVoices; ++i)
    {
        Voice& v = M.voices[i];
        if (!v.active)
            continue;
        if (v.after)
        {
            const Voice& a = M.voices[v.after - 1];
            if (a.active && a.gen == v.afterGen)
                continue; // waits for the voice before it
            v.after = 0;
        }
        float pan = 0;
        float g = dbToGain(v.volume + M.defVolume[v.def] + v.tuneDb + M.busDb[v.bus]);
        if (v.flags & FlagAt)
            g *= distanceGain(v, &pan);
        v.audible = g >= kSilent;
        if (!v.audible && !(v.flags & FlagLoop) && !v.delay)
        {
            endVoice(i); // a one-shot no one can hear
            continue;
        }
        if (v.bus == BusVoice && !v.stopping && !v.delay && v.audible)
            voiceBus = true;
        picks[np++] = { float(v.priority) * 10.0f + std::min(g, 9.0f), i };
    }
    if (np > kMixed)
        std::nth_element(picks, picks + kMixed, picks + np, [](const Pick& a, const Pick& b) { return a.score > b.score; });
    for (uint32_t k = kMixed; k < np; ++k)
    {
        Voice& v = M.voices[picks[k].i];
        if (!(v.flags & FlagLoop) && !v.stopping)
            beginStop(v, CutNow, 0); // a one-shot past the voices heard: gone in 5 ms
        else if (v.flags & FlagLoop)
            v.audible = false; // virtual: keeps its place below
    }
    // Ducking: the Voice bus lowers Music and Ambience.
    const float secs = count / rate;
    if (voiceBus)
        M.duckDb = std::max(kDuckDb, M.duckDb + kDuckDb / kDuckIn * secs);
    else
        M.duckDb = std::min(0.0f, M.duckDb - kDuckDb / kDuckOut * secs);

    float* o = out;
    for (uint32_t done = 0; done < count; done += kChunk, o += 2 * kChunk)
    {
        const uint32_t n = std::min(kChunk, count - done);
        // F1's Pause ramps every voice down and holds it where it is; a preview plays on.
        const float p0 = M.pause;
        if (M.pause != M.pauseTarget)
        {
            const float step = n / (0.01f * rate);
            M.pause = M.pauseTarget > M.pause ? std::min(M.pauseTarget, M.pause + step) : std::max(M.pauseTarget, M.pause - step);
        }
        const bool held = p0 <= 0.0f && M.pause <= 0.0f;
        for (uint32_t i = 0; i < kVoices; ++i)
        {
            Voice& v = M.voices[i];
            if (!v.active || v.after)
                continue;
            const bool preview = v.flags & FlagPreview;
            if ((held && !preview) || (v.leaving && v.env.t >= 1.0f))
                continue; // paused, or faded out for an unload: held where it is (and kept for the next version)
            uint32_t from = 0;
            if (v.delay)
            {
                const uint64_t wait = std::min<uint64_t>(v.delay, n);
                v.delay -= wait, from = uint32_t(wait);
                if (v.stopIn != ~0ull)
                    v.stopIn = v.stopIn > wait ? v.stopIn - wait : 0;
                if (from >= n)
                    continue;
            }
            if (v.stopIn != ~0ull)
            {
                if (v.stopIn <= n - from)
                    beginStop(v, CutFade, v.stopFade), v.stopIn = ~0ull;
                else
                    v.stopIn -= n - from;
            }
            float pan = 0;
            float g = dbToGain(v.volume + M.defVolume[v.def] + v.tuneDb + M.busDb[v.bus] +
                               (v.bus == BusMusic || v.bus == BusAmbience ? M.duckDb : 0.0f));
            const bool at = v.flags & FlagAt;
            if (at)
                g *= distanceGain(v, &pan);
            const float pitchRatio = powf(2.0f, (v.pitch + M.defPitch[v.def] + v.tunePitch) / 12.0f);
            // Gains at the chunk's end: envelope after it, equal-power pan, the master and pause.
            Env after = v.env;
            after.t = std::min(1.0f, after.t + after.dt * (n - from));
            const float    e1 = after.value();
            const uint32_t ch = channelsOf(v.src[v.seg]);
            float          pl = 1, pr = 1;
            if (at || ch == 1)
            {
                const float th = (pan + 1.0f) * kPi * 0.25f;
                pl = cosf(th), pr = sinf(th);
                if (ch == 2) // a stereo sound keeps its centre at full volume
                    pl = std::min(1.0f, pl * 1.4142f), pr = std::min(1.0f, pr * 1.4142f);
            }
            const float m1 = g * M.master * (preview ? 1.0f : M.pause) * e1;
            const float gl1 = v.audible ? m1 * pl : 0.0f, gr1 = v.audible ? m1 * pr : 0.0f;
            // Going virtual, a voice is heard for one more chunk, ramping down: no click.
            const bool heard = v.audible || v.gl > 0.0f || v.gr > 0.0f;
            const bool alive = advance(v, n - from, heard ? o + 2 * from : nullptr, v.gl, v.gr, gl1, gr1, pitchRatio);
            v.gl = gl1, v.gr = gr1;
            if (!alive)
                endVoice(i);
            else
                gPosition[i].store(float(v.pos / std::max(1u, rateOf(v.src[v.seg]))), std::memory_order_relaxed);
        }
        // The limiter: a chunk louder than the ceiling comes down to it at once, and back up gently.
        float peak = 0;
        for (uint32_t k = 0; k < 2 * n; ++k)
            peak = std::max(peak, fabsf(o[k]));
        const float want = peak > kCeiling ? kCeiling / peak : 1.0f;
        const float g0 = M.limiter;
        const float g1 = want < g0 ? want : std::min(want, g0 + (1.0f - g0) * std::min(1.0f, n / (0.1f * rate)));
        for (uint32_t k = 0; k < n; ++k)
        {
            const float g = want < g0 ? g1 : g0 + (g1 - g0) * (k + 1.0f) / n;
            for (int c = 0; c < 2; ++c)
            {
                const float x = o[2 * k + c] * g;
                o[2 * k + c] = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
            }
        }
        M.limiter = g1;
    }
    gCallbacks.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mix
} // namespace sound
