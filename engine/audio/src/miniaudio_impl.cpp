// The miniaudio implementation (single translation unit, compiled with warnings off: third-party code).
// Only the low-level device API and the decoders are compiled in; Fledge has its own mixer.
// Ogg Vorbis: stb_vorbis (public domain, shipped in miniaudio's extras/) is included header-only before
// miniaudio so ma_decoder detects it, and its implementation follows miniaudio's (miniaudio's documented order).
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
