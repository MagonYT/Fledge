// DEPRECATED (1.0.1): Perch was renamed to Fledge Script - include <fledge/script/script.h> and use fledge::script.
// This header keeps scripts written for earlier versions compiling: the old namespace and macros are aliases.
#pragma once
#include "fledge/script/script.h"

namespace fledge {
namespace perch = script;  // `using namespace fledge::perch;` and `fledge::perch::Entity` keep working
}

#define PERCH_PROPERTY FLEDGE_PROPERTY
#define PERCH_BEHAVIOUR FLEDGE_BEHAVIOUR
#define PERCH_SERVICE FLEDGE_SERVICE
