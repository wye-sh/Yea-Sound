// The Sound component's hook-up (components/sound/AGENTS.md). A module that plays sounds includes it after
// its own yea_<module>.h:
//
//   #include "../../components/sound/modules/sound/SoundLink.h"
//
// and needs, in its module.yea, `import sound u64 api(u32 version) = 0`. Each SOUND line gets every knob of
// snd::kKnobSpecs from the sound module's each block (settings.<name>_<suffix>, tuned in the Sound Editor), with no
// line of its own. Sounds are declared inside the module's namespace, a file each from its Sounds/ folder:
//
//   SOUND(jump, "max 4 cooldown 0.05")
//   #embed "Sounds/jump.wav"
//   SOUND_END
//
// SOUND_AND adds a variation (one is picked at random per play), SOUND_INTRO and SOUND_OUTRO a loop's intro
// and outro (each followed by its #embed line). SOUND_VERSION(jump, big) adds a version: the same files
// with knobs of its own (settings.jump_big_volume, ...), played as snd::play(jump_big). Then snd::play(jump),
// snd::play(jump, pos), and the rest below. Call snd::update() once per update(): it brings the knobs to the
// playing sounds.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wc23-extensions" // #embed, standard in C23 and C++26

namespace snd
{
constexpr uint32_t kApi = 2; // raised when Api or what it takes changes shape (the sound module serves the older ones too)

enum class Bus : uint32_t { Effects, Interface, Voice, Ambience, Music }; // snd::Bus::Music
// How a voice stops: as its sound line says (Default), at once (a 5 ms ramp), fading over some seconds,
// into its outro at once, or at the end of its loop pass (then its outro, if any).
enum Cut : uint32_t { Default, Now, Fade, Outro, Release };

using Voice = uint32_t; // a playing sound; 0 is none. Fits a u32 table field and outlives code reloads.

// A sound's knobs. Live ones the mixer applies as it plays (a change is heard at once, nothing is rendered);
// baked ones render the sound's files again, once per set of values, and apply from the next play.
enum Knob : uint32_t
{
    KnobVolume, KnobPitch, KnobPan, KnobLowpass, KnobHighpass,                                          // live
    KnobSpeed, KnobBass, KnobTreble, KnobEcho, KnobReverb, KnobStart, KnobEnd, KnobFadeIn, KnobFadeOut, // baked
    kKnobs
};
constexpr uint32_t kLive = KnobSpeed; // the first baked one
struct KnobSpec
{
    const char* suffix; // the setting is <sound>_<suffix> (a version's: <sound>_<version>_<suffix>), as module.yea's each block makes it
    const char* title;
    float       def, min, max; // max 0: open-ended
};
inline constexpr KnobSpec kKnobSpecs[kKnobs] = {
    { "volume", "Volume", 0, -40, 6 },      // dB
    { "pitch", "Pitch", 0, -12, 12 },       // semitones, like tape: higher is also quicker
    { "pan", "Pan", 0, -1, 1 },             // left -1 to right 1
    { "lowpass", "Muffle", 0, 0, 1 },       // a low-pass filter: 20 kHz down to 150 Hz
    { "highpass", "Thin", 0, 0, 1 },        // a high-pass filter: 20 Hz up to 3 kHz
    { "speed", "Speed", 1, 0.5f, 2 },       // tempo at the same pitch
    { "bass", "Bass", 0, -12, 12 },         // dB, a shelf below 200 Hz
    { "treble", "Treble", 0, -12, 12 },     // dB, a shelf above 4 kHz
    { "echo", "Echo", 0, 0, 1 },            // repeats 0.28 s apart
    { "reverb", "Reverb", 0, 0, 1 },        // a room, small to large
    { "start", "Trim Start", 0, 0, 0 },     // seconds cut from its start
    { "end", "Trim End", 0, 0, 0 },         // seconds cut from its end
    { "fade_in", "Fade In", 0, 0, 0 },      // seconds
    { "fade_out", "Fade Out", 0, 0, 0 },    // seconds
};

// A game's own values for a play, for knobs that are truly arbitrary (a voice per generated character).
// Unset ones (NaN) are the sound's; set ones replace them.
struct Knobs
{
    float volume = __builtin_nanf(""), pitch = __builtin_nanf(""), pan = __builtin_nanf(""), lowpass = __builtin_nanf(""),
          highpass = __builtin_nanf(""), speed = __builtin_nanf(""), bass = __builtin_nanf(""), treble = __builtin_nanf(""),
          echo = __builtin_nanf(""), reverb = __builtin_nanf(""), start = __builtin_nanf(""), end = __builtin_nanf(""),
          fade_in = __builtin_nanf(""), fade_out = __builtin_nanf("");
};
static_assert(sizeof(Knobs) == sizeof(float) * kKnobs, "Knobs is kKnobs floats in Knob order");

// What crosses to the sound module.
struct File
{
    uint32_t             kind; // 0 a variation, 1 the intro, 2 the outro
    uint32_t             pad;
    const unsigned char* bytes;
    uint64_t             size;
};
struct Spec
{
    const char* owner; // the module's folder name
    const char* name;
    const char* version; // "" the sound itself, else one of its versions
    const char* options;
    const File* files;
    uint32_t    fileCount;
    uint32_t    knobMask; // the knobs its module declared (1 << Knob)
    float       knobs[kKnobs];
};
struct Play
{
    float        at[3];
    uint32_t     flags; // 1 positional, 2 a sting (waits for the music's next beat)
    float        volume, pitch; // added to its knobs and jitter
    const float* knobs;         // kKnobs of the game's own (NaN: the sound's), or nullptr
};
// For a panel: one registered sound.
struct Info
{
    const char* name;
    const char* owner;
    const char* options;
    const char* version;               // "" the sound itself, else a version of it
    uint32_t    id, bus, flags, files; // flags: 1 loop, 2 music, 4 streamed (baked knobs don't apply)
    uint32_t    voices;                // playing now
    uint32_t    of;                    // a version's sound (its id), else 0
    uint32_t    knobMask;              // the knobs its module declared (1 << Knob)
};
// One file of a sound, for drawing it.
struct Wave
{
    uint32_t     kind; // as File's
    uint32_t     channels, rate;
    uint32_t     peaksReady;    // 0 while a streamed file is still being scanned
    float        seconds;
    const float* peaks;         // 256 (min, max) pairs over the whole file
};

struct Api
{
    uint32_t version, size;
    uint64_t instance; // changes whenever the sound module starts: its sounds are registered again
    void (*begin)(const char* owner);
    uint32_t (*define)(const Spec* spec, char* error, uint32_t errorBytes); // its id, or 0 and why
    void (*end)(const char* owner); // the owner's sounds not defined since begin() fade out and go
    void (*knobs)(uint32_t sound, const float* values); // kKnobs
    Voice (*play)(uint32_t sound, const Play* p);
    void (*stop)(Voice v, uint32_t cut, float seconds);
    void (*move)(Voice v, const float* at);
    void (*tune)(Voice v, float volumeDb, float pitch);
    int (*playing)(Voice v);
    void (*music)(uint32_t sound); // 0: the music fades out
    void (*listener)(const float* at, const float* forward, const float* up);
    void (*bus)(uint32_t bus, float volumeDb);
    void (*set)(Voice v, const float* values); // a playing voice's live knobs (kKnobs; NaN keeps one)
    // For a panel (the Sound Editor): every sound, in order, and a preview that plays through Pause.
    uint32_t (*count)(void);
    int (*info)(uint32_t index, Info* out);
    int (*wave)(uint32_t sound, uint32_t file, Wave* out); // as its baked knobs render it
    // One file alone on the Interface bus, from `from` seconds; flags 1 repeats it, 2 starts it held. Plays
    // through Pause, outside cooldown, limits and the no-repeat rule. One at a time: a new one ends the last.
    Voice (*preview)(uint32_t sound, uint32_t file, float from, uint32_t flags);
    float (*position)(Voice v); // seconds into the part playing
    void (*hold)(Voice v, int on);   // held where it is (a 10 ms ramp), or on again
    void (*repeat)(Voice v, int on); // loops its part, or plays it out
};

// ---- Declaring sounds (the SOUND macros) ----

struct Sound;
namespace detail
{
struct Part
{
    const Sound*         sound;
    uint32_t             kind;
    const unsigned char* bytes;
    size_t               size;
    Part*                next;
};
inline Part*    gParts = nullptr;  // this module's, newest first
inline Sound*   gSounds = nullptr; // likewise
inline uint64_t gInstance = 0;     // the sound module's instance they were registered with
inline bool     gWarned = false;

struct AddPart
{
    Part part;
    AddPart(const Sound* s, uint32_t kind, const unsigned char* bytes, size_t size) : part{ s, kind, bytes, size, gParts }
    {
        gParts = &part;
    }
};
} // namespace detail

struct Sound
{
    const char*         name;
    const char*         version; // "" the sound, else this version's name
    const Sound*        of;      // a version's sound, else nullptr
    const char*         options;
    const float* const* knob;    // its kKnobs settings
    const char*         file;    // where it was declared: its module's folder names its owner
    uint32_t            id = 0;
    float               sent[kKnobs] = {};
    Sound*              next = nullptr;
    Sound(const char* n, const char* v, const Sound* o, const char* opts, const float* const* k, const char* f)
        : name(n), version(v), of(o), options(opts), knob(k), file(f)
    {
        next = detail::gSounds, detail::gSounds = this;
    }
    Sound(const Sound&) = delete;
    float value(uint32_t k) const { return *knob[k]; }
};

namespace detail
{
// "…/modules/orbbit_fx/Sounds.cpp" -> "orbbit_fx"
inline void ownerOf(const char* file, char* out, size_t bytes)
{
    const char* end = nullptr;
    const char* start = file;
    for (const char* p = file; *p; ++p)
        if (*p == '/' || *p == '\\')
            start = end ? end + 1 : file, end = p;
    size_t n = end ? size_t(end - start) : 0;
    n = n < bytes - 1 ? n : bytes - 1;
    memcpy(out, start, n);
    out[n] = 0;
}

// Each sound from `s` on, the oldest first (gSounds is newest first): the registry, and a panel, keep the order declared.
inline void define(const Api* a, Sound* s, const char* owner)
{
    if (!s)
        return;
    define(a, s->next, owner);
    {
        File     files[40];
        uint32_t n = 0;
        for (Part* p = gParts; p; p = p->next)
            if (p->sound == s && n < 40)
                files[n++] = File{ p->kind, 0, p->bytes, p->size };
        for (Part* p = gParts; p && !n && s->of; p = p->next) // a version with no files of its own: its sound's
            if (p->sound == s->of && n < 40)
                files[n++] = File{ p->kind, 0, p->bytes, p->size };
        for (uint32_t i = 0; i < n / 2; ++i) // declared order
        {
            const File t = files[i];
            files[i] = files[n - 1 - i], files[n - 1 - i] = t;
        }
        Spec spec = { owner, s->name, s->version, s->options, files, n, 0, {} };
        for (uint32_t k = 0; k < kKnobs; ++k)
            spec.knobs[k] = s->sent[k] = s->value(k), spec.knobMask |= 1u << k;
        char error[200] = "";
        s->id = a->define(&spec, error, sizeof(error));
        if (!s->id)
        {
            char message[300];
            snprintf(message, sizeof(message), "sound %s%s%s: %s", s->name, s->version[0] ? " version " : "", s->version, error);
            yea::host()->fail(message); // reported and reverted, like a crash
        }
    }
}
// Registers every sound this module declares with the sound module running now.
inline void attach(const Api* a)
{
    gInstance = a->instance;
    if (!gSounds)
        return;
    char owner[64];
    ownerOf(gSounds->file, owner, sizeof(owner));
    a->begin(owner);
    define(a, gSounds, owner);
    a->end(owner);
}

// The sound module's calls, with this module's sounds registered; nullptr while it isn't running.
inline const Api* api()
{
    const Api* a = (const Api*)(uintptr_t)::sound::api(kApi);
    if (!a)
    {
        if (!gWarned && ::sound::api(0))
            yea::host()->log(1, "the Sound component is a different version: rebuild this module"), gWarned = true;
        return nullptr;
    }
    if (a->instance != gInstance)
        attach(a);
    return a;
}
} // namespace detail

#define SND_CAT2(a, b) a##b
#define SND_CAT(a, b) SND_CAT2(a, b)
#define SND_OPEN(k) namespace SND_CAT(file_, __LINE__) { static const uint32_t kKind = k; static const unsigned char bytes[] = {
#define SND_CLOSE }; static ::snd::detail::AddPart part(&sound, kKind, bytes, sizeof bytes); }

// A sound's knob settings, in Knob order, as the sound module's each block makes them for its SOUND line.
#define SND_KNOBS(p)                                                                                                   \
    { &settings.p##_volume, &settings.p##_pitch, &settings.p##_pan, &settings.p##_lowpass, &settings.p##_highpass,      \
      &settings.p##_speed, &settings.p##_bass, &settings.p##_treble, &settings.p##_echo, &settings.p##_reverb,          \
      &settings.p##_start, &settings.p##_end, &settings.p##_fade_in, &settings.p##_fade_out }

// SOUND(name, "options"), then each file's #embed line, then SOUND_END. The handle is `name`; its knobs are
// settings.name_volume and the rest of kKnobSpecs.
#define SOUND(name, options)                                                                                       \
    namespace name##_sound { extern ::snd::Sound sound; }                                                          \
    ::snd::Sound& name = name##_sound::sound;                                                                      \
    namespace name##_sound {                                                                                       \
    static const char* const          kName = #name;                                                               \
    static const char* const          kVersion = "";                                                               \
    static const ::snd::Sound* const  kOf = nullptr;                                                               \
    static const char* const          kOptions = options;                                                          \
    static const float* const         kKnobs[] = SND_KNOBS(name);                                                  \
    SND_OPEN(0)
#define SOUND_AND   SND_CLOSE SND_OPEN(0)
#define SOUND_INTRO SND_CLOSE SND_OPEN(1)
#define SOUND_OUTRO SND_CLOSE SND_OPEN(2)
#define SOUND_END   SND_CLOSE ::snd::Sound sound(kName, kVersion, kOf, kOptions, kKnobs, __FILE__); }

// A version of a sound (after its SOUND_END, in the same file): the same files and options, knobs of its own
// (settings.name_version_volume, ...), played as snd::play(name_version). Not a variation: the game chooses it.
#define SOUND_VERSION(name, version)                                                                               \
    namespace name##_##version##_sound {                                                                           \
    static const float* const kKnobs[] = SND_KNOBS(name##_##version);                                              \
    ::snd::Sound sound(#name, #version, &name##_sound::sound, name##_sound::kOptions, kKnobs, __FILE__);           \
    }                                                                                                              \
    ::snd::Sound& name##_##version = name##_##version##_sound::sound;
// A version with files of its own (an edited copy of the sound's): its #embed lines follow, then SOUND_END.
#define SOUND_VERSION_FILES(name, version)                                                                         \
    namespace name##_##version##_sound { extern ::snd::Sound sound; }                                              \
    ::snd::Sound& name##_##version = name##_##version##_sound::sound;                                              \
    namespace name##_##version##_sound {                                                                           \
    static const char* const          kName = #name;                                                               \
    static const char* const          kVersion = #version;                                                         \
    static const ::snd::Sound* const  kOf = &name##_sound::sound;                                                  \
    static const char* const          kOptions = name##_sound::kOptions;                                           \
    static const float* const         kKnobs[] = SND_KNOBS(name##_##version);                                      \
    SND_OPEN(0)

// ---- Playing them ----

// Flat (no position): music, the interface, the player's own sounds.
inline Voice play(const Sound& s)
{
    const Api* a = detail::api();
    return a ? a->play(s.id, nullptr) : 0;
}
// At a place in the world: panned and quieter with distance from the listener.
inline Voice play(const Sound& s, yea_float3 at)
{
    const Api* a = detail::api();
    const Play p = { { at.x, at.y, at.z }, 1, 0, 0, nullptr };
    return a ? a->play(s.id, &p) : 0;
}
inline Voice play(const Sound& s, yea_float2 at) { return play(s, yea_float3{ at.x, at.y, 0.0f }); }
// With a volume (dB) and pitch (semitones) of its own on top of its knobs, flat or positional.
inline Voice play(const Sound& s, float volumeDb, float pitch)
{
    const Api* a = detail::api();
    const Play p = { {}, 0, volumeDb, pitch, nullptr };
    return a ? a->play(s.id, &p) : 0;
}
inline Voice play(const Sound& s, yea_float3 at, float volumeDb, float pitch)
{
    const Api* a = detail::api();
    const Play p = { { at.x, at.y, at.z }, 1, volumeDb, pitch, nullptr };
    return a ? a->play(s.id, &p) : 0;
}
// With the game's own values for some knobs (the rest are the sound's): a voice per generated character.
// Live ones cost nothing; baked ones render the files once per set of values (kept while the view runs).
inline Voice play(const Sound& s, const Knobs& k)
{
    const Api* a = detail::api();
    const Play p = { {}, 0, 0, 0, &k.volume };
    return a ? a->play(s.id, &p) : 0;
}
inline Voice play(const Sound& s, yea_float3 at, const Knobs& k)
{
    const Api* a = detail::api();
    const Play p = { { at.x, at.y, at.z }, 1, 0, 0, &k.volume };
    return a ? a->play(s.id, &p) : 0;
}
inline Voice play(const Sound& s, yea_float2 at, const Knobs& k) { return play(s, yea_float3{ at.x, at.y, 0.0f }, k); }
// A playing voice's live knobs (volume, pitch, pan, lowpass, highpass), on top of nothing: they replace its
// sound's. Baked ones apply from its next play.
inline void set(Voice v, const Knobs& k)
{
    if (const Api* a = detail::api())
        a->set(v, &k.volume);
}
// On the music's next beat (at once with no music).
inline Voice sting(const Sound& s)
{
    const Api* a = detail::api();
    const Play p = { {}, 2, 0, 0, nullptr };
    return a ? a->play(s.id, &p) : 0;
}
// Its sound line's cut-off, or another: stop(v, snd::Now), stop(v, snd::Fade, 0.5), stop(v, snd::Release).
inline void stop(Voice v, Cut cut = Default, float seconds = 0)
{
    if (const Api* a = detail::api())
        a->stop(v, cut, seconds);
}
inline void move(Voice v, yea_float3 at)
{
    const float p[3] = { at.x, at.y, at.z };
    if (const Api* a = detail::api())
        a->move(v, p);
}
inline void move(Voice v, yea_float2 at) { move(v, yea_float3{ at.x, at.y, 0.0f }); }
// Volume (dB) and pitch (semitones) on top of its knobs, live: an engine's revs.
inline void tune(Voice v, float volumeDb, float pitch)
{
    if (const Api* a = detail::api())
        a->tune(v, volumeDb, pitch);
}
// True until it has fully stopped (through its fade or outro).
inline bool playing(Voice v)
{
    const Api* a = detail::api();
    return a && a->playing(v);
}
// The view's music: the same again changes nothing, so it is fine every frame. music() fades it out.
inline void music(const Sound& s)
{
    if (const Api* a = detail::api())
        a->music(s.id);
}
inline void music()
{
    if (const Api* a = detail::api())
        a->music(0);
}
// Where the listener is: the module that owns the camera calls it. A 2D game: the camera's centre.
inline void listener(yea_float2 at)
{
    const float p[3] = { at.x, at.y, 0 }, f[3] = { 0, 1, 0 }, u[3] = { 0, 0, 1 };
    if (const Api* a = detail::api())
        a->listener(p, f, u);
}
inline void listener(yea_float3 at, yea_float3 forward, yea_float3 up)
{
    const float p[3] = { at.x, at.y, at.z }, f[3] = { forward.x, forward.y, forward.z }, u[3] = { up.x, up.y, up.z };
    if (const Api* a = detail::api())
        a->listener(p, f, u);
}
// A bus's volume (dB) from the game's own options, on top of the component's knobs.
inline void bus(Bus b, float volumeDb)
{
    if (const Api* a = detail::api())
        a->bus(uint32_t(b), volumeDb);
}
// Once per update(): registers the sounds when the sound module (re)starts, and sends knob changes.
inline void update()
{
    const Api* a = detail::api();
    if (!a)
        return;
    for (Sound* s = detail::gSounds; s; s = s->next)
    {
        if (!s->id)
            continue;
        float v[kKnobs];
        bool  changed = false;
        for (uint32_t k = 0; k < kKnobs; ++k)
            v[k] = s->value(k), changed |= v[k] != s->sent[k];
        if (changed)
            a->knobs(s->id, v), memcpy(s->sent, v, sizeof(v));
    }
}
} // namespace snd
