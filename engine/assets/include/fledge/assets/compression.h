#pragma once
// Fledge-LZ: a tiny byte-oriented LZ77 codec (no external dependency).
// Stream of tokens:
//   0x00..0x7F : literal run, (c+1) raw bytes follow (1..128)
//   0x80..0xFF : match, length (c&0x7F)+3 (3..130), followed by u16 LE back-offset (1..65535)
// Decent on text/JSON/OBJ, near-zero gain on PNG. Roadmap: LZ4/zstd.
#include <optional>
#include <span>

#include "fledge/core/utility.h"

namespace fledge::lz {

Bytes compress(std::span<const uint8_t> input);
// Returns nullopt on malformed input or if the output size != expectedSize.
std::optional<Bytes> decompress(std::span<const uint8_t> input, size_t expectedSize);

} // namespace fledge::lz
