#include "gl_loader.h"

namespace fledge::gl {

#define FLEDGE_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
FLEDGE_GL_FUNCS(FLEDGE_GL_DEFINE)
#undef FLEDGE_GL_DEFINE

const char* loadFunctions(void* (*getProc)(const char*)) {
#define FLEDGE_GL_LOAD(ret, name, ...) \
    name = reinterpret_cast<PFN_##name>(getProc(#name)); \
    if (!name) return #name;
    FLEDGE_GL_FUNCS(FLEDGE_GL_LOAD)
#undef FLEDGE_GL_LOAD
    return nullptr;
}

} // namespace fledge::gl
