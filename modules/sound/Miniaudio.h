// miniaudio as the Sound component builds it (third_party/miniaudio, Unlicense OR MIT-0): only the
// device, the decoders (Ogg Vorbis through stb_vorbis, WAV, FLAC) and the backends games play on.
#pragma once

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_COREAUDIO // macOS, when games build there
#define MA_ENABLE_NULL      // no sound device: it keeps time, so voices still end
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_MP3 // its padding breaks loops
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH

// Windows: playback joins the session Session.cpp names (third_party/miniaudio is changed to take it).
extern const void* soundSession; // a GUID, or nullptr: miniaudio's own (one session per process)
#define MA_WASAPI_SESSION_GUID  ((const GUID*)soundSession)
#define MA_WASAPI_SESSION_FLAGS (soundSession ? 0x00010000u : 0u) /* AUDCLNT_STREAMFLAGS_CROSSPROCESS */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "third_party/miniaudio/miniaudio.h"
#pragma clang diagnostic pop
