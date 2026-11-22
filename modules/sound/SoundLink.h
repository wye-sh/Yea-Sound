// The Sound component's hook-up (components/sound/AGENTS.md). A module that plays sounds includes it after
// its own yea_<module>.h:
//
//   #include "../../components/sound/modules/sound/SoundLink.h"
//
// and needs, in its module.yea, `import sound u64 api(u32 version) = 0`, plus a Volume and a Pitch knob per
// sound (`setting float jump_volume 0 -40 6` and `setting float jump_pitch 0 -12 12`, under
// `category Sound`). Sounds are declared inside the module's namespace, a file each from its Sounds/ folder:
//
//   SOUND(jump, "max 4 cooldown 0.05")
//   #embed "Sounds/jump.wav"
//   SOUND_END
//
// SOUND_AND adds a variation, SOUND_INTRO and SOUND_OUTRO a loop's intro and outro (each followed by its
// #embed line). Then snd::play(jump), snd::play(jump, pos), and the rest below. Call snd::update() once
// per update(): it brings the knobs to the playing sounds.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wc23-extensions" // #embed, standard in C23 and C++26

namespace snd
{
constexpr uint32_t kApi = 1; // raised when Api or what it takes changes shape

enum class Bus : uint32_t { Effects, Interface, Voice, Ambience, Music }; // snd::Bus::Music
// How a voice stops: as its sound line says (Default), at once (a 5 ms ramp), fading over some seconds,
// into its outro at once, or at the end of its loop pass (then its outro, if any).
enum Cut : uint32_t { Default, Now, Fade, Outro, Release };

using Voice = uint32_t; // a playing sound; 0 is none. Fits a u32 table field and outlives code reloads.

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
    const char* options;
    const File* files;
    uint32_t    fileCount;
    float       volume, pitch; // its knobs
};
struct Play
{
    float    at[3];
    uint32_t flags; // 1 positional, 2 a sting (waits for the music's next beat)
    float    volume, pitch; // added to its knobs and jitter
};
// For a panel: one registered sound.
struct Info
{
    const char* name;
    const char* owner;
    const char* options;
    uint32_t    id, bus, flags, files; // flags: 1 loop, 2 music, 4 streamed
    uint32_t    voices;                // playing now
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
    void (*knobs)(uint32_t sound, float volumeDb, float pitch);
    Voice (*play)(uint32_t sound, const Play* p);
    void (*stop)(Voice v, uint32_t cut, float seconds);
    void (*move)(Voice v, const float* at);
    void (*tune)(Voice v, float volumeDb, float pitch);
    int (*playing)(Voice v);
    void (*music)(uint32_t sound); // 0: the music fades out
    void (*listener)(const float* at, const float* forward, const float* up);
    void (*bus)(uint32_t bus, float volumeDb);
    // For a panel (the Sound Editor): every sound, in order, and a preview that plays through Pause.
    uint32_t (*count)(void);
    int (*info)(uint32_t index, Info* out);
    int (*wave)(uint32_t sound, uint32_t file, Wave* out);
    Voice (*preview)(uint32_t sound, uint32_t file);
    float (*position)(Voice v); // seconds into the part playing
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
    const char*  name;
    const char*  options;
    const float* volume; // its knobs
    const float* pitch;
    const char*  file; // where it was declared: its module's folder names its owner
    uint32_t     id = 0;
    float        sentVolume = 0, sentPitch = 0;
    Sound*       next = nullptr;
    Sound(const char* n, const char* o, const float* v, const float* p, const char* f) : name(n), options(o), volume(v), pitch(p), file(f)
    {
        next = detail::gSounds, detail::gSounds = this;
    }
    Sound(const Sound&) = delete;
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
        for (uint32_t i = 0; i < n / 2; ++i) // declared order
        {
            const File t = files[i];
            files[i] = files[n - 1 - i], files[n - 1 - i] = t;
        }
        const Spec spec = { owner, s->name, s->options, files, n, *s->volume, *s->pitch };
        char       error[200] = "";
        s->id = a->define(&spec, error, sizeof(error));
        s->sentVolume = *s->volume, s->sentPitch = *s->pitch;
        if (!s->id)
        {
            char message[300];
            snprintf(message, sizeof(message), "sound %s: %s", s->name, error);
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

// SOUND(name, "options"), then each file's #embed line, then SOUND_END. The handle is `name`; its knobs are
// settings.name_volume and settings.name_pitch, which must exist.
#define SOUND(name, options)                                                                                       \
    namespace name##_sound { extern ::snd::Sound sound; }                                                          \
    ::snd::Sound& name = name##_sound::sound;                                                                      \
    namespace name##_sound {                                                                                       \
    static const char* const  kName = #name;                                                                       \
    static const char* const  kOptions = options;                                                                  \
    static const float* const kVolume = &settings.name##_volume;                                                   \
    static const float* const kPitch = &settings.name##_pitch;                                                     \
    SND_OPEN(0)
#define SOUND_AND   SND_CLOSE SND_OPEN(0)
#define SOUND_INTRO SND_CLOSE SND_OPEN(1)
#define SOUND_OUTRO SND_CLOSE SND_OPEN(2)
#define SOUND_END   SND_CLOSE ::snd::Sound sound(kName, kOptions, kVolume, kPitch, __FILE__); }

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
    const Play p = { { at.x, at.y, at.z }, 1, 0, 0 };
    return a ? a->play(s.id, &p) : 0;
}
inline Voice play(const Sound& s, yea_float2 at) { return play(s, yea_float3{ at.x, at.y, 0.0f }); }
// With a volume (dB) and pitch (semitones) of its own on top of its knobs, flat or positional.
inline Voice play(const Sound& s, float volumeDb, float pitch)
{
    const Api* a = detail::api();
    const Play p = { {}, 0, volumeDb, pitch };
    return a ? a->play(s.id, &p) : 0;
}
inline Voice play(const Sound& s, yea_float3 at, float volumeDb, float pitch)
{
    const Api* a = detail::api();
    const Play p = { { at.x, at.y, at.z }, 1, volumeDb, pitch };
    return a ? a->play(s.id, &p) : 0;
}
// On the music's next beat (at once with no music).
inline Voice sting(const Sound& s)
{
    const Api* a = detail::api();
    const Play p = { {}, 2, 0, 0 };
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
        if (s->id && (*s->volume != s->sentVolume || *s->pitch != s->sentPitch))
        {
            a->knobs(s->id, *s->volume, *s->pitch);
            s->sentVolume = *s->volume, s->sentPitch = *s->pitch;
        }
}
} // namespace snd
