// The mixer (Mixer.cpp): runs on the sound device's thread. It reads only commands from the ring and
// memory the main thread made before sending them and frees only after the mixer has let go of it. It
// never calls the host or a module, allocates, locks or waits. Everything else in the component is the
// main thread's (Sound.cpp).
#pragma once

#include <atomic>
#include <stdint.h>

namespace sound
{
namespace mix
{
constexpr uint32_t kVoices = 256;     // tracked at once
constexpr uint32_t kMixed = 64;       // heard at once; the rest are virtual (loops) or end (one-shots)
constexpr uint32_t kDefs = 1024;      // sounds defined at once
constexpr uint32_t kStreams = 4;      // streamed sounds playing at once
constexpr uint32_t kRing = 1u << 17;  // frames per stream ring (2.7 s at 48 kHz)
constexpr uint32_t kCommands = 4096;
constexpr float    kRamp = 0.005f;    // the shortest fade: no click

enum Bus : uint32_t { BusEffects, BusInterface, BusVoice, BusAmbience, BusMusic, BusCount };
enum Cut : uint32_t { CutDefault, CutNow, CutFade, CutOutro, CutRelease };
enum Switch : uint32_t { SwitchBar, SwitchBeat, SwitchNow, SwitchEnd };
enum Flag : uint32_t
{
    FlagLoop = 1,
    FlagMusic = 2,
    FlagAt = 4,      // positional
    FlagSting = 8,   // waits for the music's next beat
    FlagPreview = 16 // plays through F1's Pause
};

// Decoded frames, interleaved float. Never changed once a command names it.
struct Buffer
{
    float*   frames;
    uint32_t count, channels, rate;
};

enum : uint32_t
{
    StreamFree,
    StreamReserved, // main: a play is being queued with it
    StreamOpen,     // main: decoding not started yet; the mixer waits, silent
    StreamReady,    // main decodes into the ring; the mixer reads
    StreamClose,    // mixer: its voice ended; main closes it
};
struct Stream
{
    std::atomic<uint32_t> state{ StreamFree };
    uint32_t              channels = 2, rate = 48000; // set before Ready
    float                 frames[kRing * 2];
    std::atomic<uint64_t> written{ 0 }, read{ 0 }; // absolute frames
    std::atomic<uint64_t> ended{ ~0ull };          // where the file ended (not looping), else ~0
    std::atomic<uint64_t> wraps[4] = {};           // where its last loop passes began (0: none)
    std::atomic<uint32_t> wrapCount{ 0 };
};
extern Stream gStreams[kStreams];

struct Source
{
    const Buffer* buffer = nullptr; // decoded
    int           stream = -1;      // else a stream slot
};

enum : uint8_t
{
    CmdPlay,
    CmdStop,
    CmdMove,
    CmdTune,
    CmdKnobs,
    CmdBus,
    CmdListener,
    CmdPause,
    CmdMaster,
    CmdMusicOff,
    CmdLeave, // every voice fades out fast, before the device stops (unload)
};
struct Command
{
    uint8_t  type;
    uint8_t  bus, priority, cut;
    uint32_t slot, gen, def;
    uint32_t flags;      // Flag
    float    a[3];       // position; listener at
    float    b[3], c[3]; // listener forward, up
    float    volume, pitch, fade, fadeIn, nearDist, farDist, bpm, crossfade;
    uint32_t beats, switchAt;
    uint32_t music, musicGen; // the music playing (slot+1, gen), for a sting
    uint32_t seg;             // where it starts: 0 intro, 1 body, 2 outro (a voice resumed after a swap)
    double   pos;             // frames into that segment
    double   issued;          // seconds (steady clock)
    Source   src[3];          // intro, body, outro
};
bool push(const Command& c); // main thread; false when the ring is full

// The device's callback: `count` stereo frames.
void render(float* out, uint32_t count);
// Main thread, while no device runs: a clean mixer at `rate`.
void reset(uint32_t rate);
void setRate(uint32_t rate); // main thread, while no device runs: a reopened device's rate

// Written by the mixer, read by the main thread.
extern std::atomic<uint32_t> gDoneGen[kVoices];  // the gen of the voice that last ended in each slot
extern std::atomic<int>      gDefVoices[kDefs];   // voices of each sound not ended yet (main up, mixer down)
extern std::atomic<uint64_t> gCallbacks;          // device callbacks so far (the watchdog's)
extern std::atomic<float>    gPosition[kVoices];  // seconds into each voice's current part

// A voice as it stands, read after the device stopped (for keeping loops across a swap).
struct State
{
    bool     active, stopping, preview;
    uint32_t gen, def, seg, flags;
    double   pos, musicSecs;
    float    at[3], tuneDb, tunePitch;
};
bool state(uint32_t slot, State* out);
} // namespace mix
} // namespace sound
