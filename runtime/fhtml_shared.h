#pragma once
#include <cstdint>

// Both processes use this fixed layout. The owner of lock accesses the frame
// and input fields. The parent initializes the HTML before launching CEF.
namespace fledge::fhtml {
inline constexpr int width = 1024;
inline constexpr int height = 576;
inline constexpr int htmlCapacity = 192 * 1024;
inline constexpr int cacheCapacity = 1024;
inline constexpr int stateCapacity = 8192;
inline constexpr int controlKeys[] = {256, 257, 258, 259, 261, 262, 263, 264, 265, 268, 269};
struct Shared {
    volatile long lock;
    volatile long stop;
    uint32_t htmlSize;
    uint32_t serial;
    int mouseX, mouseY;
    bool mouseDown;
    uint32_t actionSerial;
    char action[96];
    char html[htmlCapacity];
    uint8_t bgra[width * height * 4];
    uint32_t keys;
    uint32_t textSize;
    char text[256];
    char16_t cacheDirectory[cacheCapacity];
    uint32_t stateSerial;
    uint32_t stateSize;
    char state[stateCapacity];
};
} // namespace fledge::fhtml
