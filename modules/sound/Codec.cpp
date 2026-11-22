// miniaudio's and stb_vorbis's code, compiled once (Miniaudio.h has the options). stb_vorbis's header
// first, so miniaudio decodes Ogg Vorbis with it. Neither is ours: their warnings are off.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "Miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis/stb_vorbis.c"
#pragma clang diagnostic pop

const void* soundSession = nullptr;

namespace sound
{
void nap(unsigned ms) { ma_sleep(ms); } // miniaudio's, on every platform
} // namespace sound
