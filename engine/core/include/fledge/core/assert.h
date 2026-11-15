#pragma once
#include "fledge/core/log.h"

#include <cstdlib>

namespace fledge::detail {
[[noreturn]] inline void assertFail(const char* expr, const char* file, int line, const char* msg) {
    Logger::get().log(LogLevel::Error, "assert",
                      std::format("Assertion failed: {} ({}:{}) {}", expr, file, line, msg ? msg : ""));
    std::abort();
}
} // namespace fledge::detail

#define FLEDGE_ASSERT(expr) \
    do { if (!(expr)) ::fledge::detail::assertFail(#expr, __FILE__, __LINE__, nullptr); } while (0)
#define FLEDGE_ASSERT_MSG(expr, msg) \
    do { if (!(expr)) ::fledge::detail::assertFail(#expr, __FILE__, __LINE__, msg); } while (0)
#ifdef NDEBUG
#define FLEDGE_DEBUG_ASSERT(expr) ((void)0)
#else
#define FLEDGE_DEBUG_ASSERT(expr) FLEDGE_ASSERT(expr)
#endif
