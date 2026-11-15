#pragma once
// AMD FidelityFX FSR 1 pass sources (see fsr1.cpp). Empty when built without the FSR headers.
#include <string>

namespace fledge::shaders {
std::string fsr1EasuHlsl();
std::string fsr1RcasHlsl();
bool fsr1Available();
} // namespace fledge::shaders
