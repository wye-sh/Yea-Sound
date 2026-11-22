// Baked knobs (Bake.cpp): a file rendered again with a sound's baked knobs. Main thread only.
#pragma once

#include "Mixer.h"

#include <stdint.h>

namespace sound
{
// The baked knobs, in snd::Knob order from KnobSpeed: speed, bass, treble, echo, reverb, start, end, fade in, fade out.
constexpr uint32_t kBaked = 9;
constexpr float    kBakedDefaults[kBaked] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
enum : uint32_t
{
    BakeFirst = 1, // starts the sound: its fade in (a one-shot, an intro)
    BakeLast = 2,  // ends it: its fade out (a one-shot, an outro)
    BakeLoop = 4,  // a loop's pass: tails wrap round onto its start
};

bool baked(const float* values); // any baked knob away from its default
// `in` rendered with `values` (kBaked) into `out` (frames malloc'd); false when out of memory.
bool bake(const mix::Buffer& in, const float* values, uint32_t role, mix::Buffer* out);
} // namespace sound
