#pragma once
// Standard edition start-up. Every executable calls initEdition() first in main().
namespace fledge {

// Sets currentEdition() to Standard. Idempotent.
void initEdition();

} // namespace fledge
