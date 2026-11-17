#pragma once
// .fpack / .fpak v2: content-addressed chunked container ("CAGEPAK2").
//
// Motivation over v1 ("CAGEPAK1", one monolithic compressed stream laid out
// entry by entry): a single changed asset used to force a full rewrite of the
// whole archive and its compression context; assets could not be compressed
// independently by type. v2 splits every entry into fixed-size chunks of raw
// data. Each chunk is compressed on its own (codec chosen per chunk by actual
// measured gain) and stored once under the FNV-1a-64 hash of its RAW bytes, so
// identical chunks (repeated assets, mip tails, serialized defaults) are
// stored a single time. Rewriting one asset only touches that asset's chunks.
//
// Layout (all integers little-endian):
//   Header (56 B): magic(8) | u32 version(2) | u32 flags | u32 chunkShift |
//                  u32 reserved | u64 entryCount | u64 chunkCount |
//                  u64 indexOffset | u64 indexSize
//   Chunks       : stored chunk payloads, each located by the chunk table
//   Index        : entry table then chunk table, see below
//
//   Entry record   : u16 pathLen | path (UTF-8, '/'-separated) |
//                    u64 rawSize | u32 chunkCount | u32 crc32(raw) |
//                    u64 chunkIndices[chunkCount]
//   Chunk record   : u64 hash | u64 offset | u32 storedSize | u32 rawSize |
//                    u8 codec | u8 reserved[3] | u32 crc32(raw)
//
// Codecs: 0 = raw, 1 = Fledge-LZ (assets/compression.h). The codec is applied
// per chunk and only kept when it measurably shrinks the payload.
//
// Appended to an executable the v2 pak reuses the v1 32-byte footer
// ("CAGEFTR1" | u64 pakOffset | u64 pakSize | u32 version(2) | u32 reserved):
// the footer version field distinguishes the payload format.
//
// The packager writes v2 atomically: header + chunks are streamed to
// "<name>.tmp", the index (written last) is only flushed to "<name>.new"
// which is then renamed over the target, so a reader never observes a partial
// archive.
//
// Packager chunk reuse: the index of an existing archive on disk can be fed
// back into FpackWriter::setChunks() via readIndex(); chunks that reappear
// (same hash) are then shared by hash instead of being re-compressed and
// re-stored.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fledge/core/utility.h"
#include "fledge/core/vfs.h"

namespace fledge {

enum class FpackCodec : uint8_t { Raw = 0, Lz = 1 };

constexpr char kFpackMagic[8] = {'C', 'A', 'G', 'E', 'P', 'A', 'K', '2'};
constexpr uint32_t kFpackVersion = 2;
// magic(8) | version(4) | flags(4) | chunkShift(4) | reserved(4) |
// entryCount(8) | chunkCount(8) | indexOffset(8) | indexSize(8) = 56
constexpr size_t kFpackHeaderSize = 56;
// Log2 of the raw chunk size (64 KiB). Every entry's data is split into
// ceil(size / 2^chunkShift) chunks, stored once per distinct raw content hash.
constexpr uint32_t kFpackChunkShift = 16;

struct FpackChunk {
    uint64_t hash = 0;       // FNV-1a-64 of the RAW chunk bytes
    uint64_t offset = 0;     // absolute offset of the stored payload in the file
    uint32_t storedSize = 0;
    uint32_t rawSize = 0;
    FpackCodec codec = FpackCodec::Raw;
    uint32_t crc = 0;        // crc32 of the RAW chunk bytes
};

struct FpackEntry {
    std::string path;
    uint64_t rawSize = 0;
    uint32_t crc = 0;
    std::vector<uint64_t> chunks;  // indices into the chunk table
};

// Serialized index = entry table followed by chunk table (built by
// FpackWriter, parsed by FpackReader; also used by the packager to plan
// incremental rewrites). Empty when there is no previous archive.
struct FpackIndex {
    std::vector<FpackEntry> entries;
    std::vector<FpackChunk> chunks;
    uint32_t chunkShift = kFpackChunkShift;
    bool empty() const { return entries.empty() && chunks.empty(); }
};

// Chooses the stored payload for one raw chunk. LZ is kept only when it saves
// at least 3% and 16 bytes; everything else stays raw so already-compressed
// content (BC textures, PNG/JPG, audio) never pays a decompression cost.
FpackCodec encodeChunk(std::span<const uint8_t> raw, Bytes& storedOut);

// Serializes an index (entry table + chunk table) as it appears at the end of
// a v2 archive. Shared by the writer and the streaming packager build.
Bytes serializeFpackIndex(const FpackIndex& idx);

// ------------------------------------------------------------- reader -----
class FpackReader {
public:
    static std::unique_ptr<FpackReader> openMemory(Bytes image);
    static std::unique_ptr<FpackReader> openFile(const fs::path& file, uint64_t offset = 0, uint64_t size = 0);
    // Opens a v2 pak appended to an executable (footer at end of file). nullptr if none.
    static std::unique_ptr<FpackReader> openAppended(const fs::path& exe);

    std::optional<Bytes> read(std::string_view path) const;  // verifies every chunk CRC + whole-entry CRC
    bool contains(std::string_view path) const { return m_index.contains(std::string(path)); }
    std::vector<std::string> list() const;
    const std::map<std::string, FpackEntry, std::less<>>& entries() const { return m_index; }
    const std::vector<FpackChunk>& chunks() const { return m_chunks; }
    uint32_t chunkShift() const { return m_chunkShift; }

private:
    bool parse(std::span<const uint8_t> header, uint64_t pakSize);
    std::optional<Bytes> readRange(uint64_t offset, uint64_t size) const;

    Bytes m_memory;           // memory mode
    fs::path m_file;          // file mode
    uint64_t m_base = 0;      // file offset of the pak start
    uint64_t m_size = 0;
    uint32_t m_chunkShift = kFpackChunkShift;
    std::vector<FpackChunk> m_chunks;
    std::map<std::string, FpackEntry, std::less<>> m_index;
    mutable std::mutex m_mutex;
};

// VFS adapter so a v2 pak can be mounted as "content:/".
class FpackProvider final : public FileProvider {
public:
    explicit FpackProvider(std::shared_ptr<FpackReader> pak) : m_pak(std::move(pak)) {}
    std::optional<Bytes> read(std::string_view p) const override { return m_pak->read(p); }
    bool exists(std::string_view p) const override { return m_pak->contains(p); }
    std::vector<std::string> list() const override { return m_pak->list(); }

private:
    std::shared_ptr<FpackReader> m_pak;
};

// ------------------------------------------------------------- writer -----
class FpackWriter {
public:
    void add(std::string path, Bytes data);
    size_t entryCount() const { return m_entries.size(); }

    // Declares chunk payloads already stored (hash -> payload) so identical
    // content is written once even across entries. Intended use: the packager
    // passes the chunks of the previous archive (payloads read from it) so
    // unchanged chunks are not re-compressed on incremental builds.
    void setChunks(const std::vector<FpackChunk>& chunks,
                   const std::unordered_map<uint64_t, Bytes>& storedByHash);

    // Streams header + chunk payloads to 'chunkOut' (opened/truncated by the
    // caller; data starts at file offset 0). Returns the serialized index;
    // persisting it (atomically) after a successful return yields a complete
    // archive. progress(done, total, path) while entries are chunked/hashed.
    Result<FpackIndex> buildIndex(std::ostream& chunkOut,
                                  const std::function<void(size_t done, size_t total, const std::string& path)>& progress = {}) const;

    // Full in-memory image: header + chunks + index. Deterministic.
    Result<Bytes> build(const std::function<void(size_t done, size_t total, const std::string& path)>& progress = {}) const;
    bool writeTo(const fs::path& file) const;

private:
    std::map<std::string, Bytes> m_entries;  // sorted, deterministic output
    std::unordered_map<uint64_t, Bytes> m_shared;  // hash -> stored payload (see setChunks)
};

// Reads only the header + index of a v2 archive (no payloads). Used by the
// packager for incremental builds; chunk payloads stay in the old file.
std::optional<FpackIndex> readFpackIndex(const fs::path& file);

// Builds the 32-byte footer that follows an appended v2 pak (same layout as
// the v1 footer, version field = 2).
Bytes makeFpackFooter(uint64_t pakOffset, uint64_t pakSize);

} // namespace fledge
