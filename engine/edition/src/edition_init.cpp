#include "fledge/edition/edition.h"

#include <mutex>

#include "fledge/core/features.h"

namespace fledge {

void initEdition() {
    static std::once_flag once;
    std::call_once(once, [] {
        setCurrentEdition(Edition::Standard);
    });
}

} // namespace fledge
