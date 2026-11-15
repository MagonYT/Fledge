#pragma once
// .fpak packed archive.
//
// Layout (all integers little-endian):
//   Header  (32 B): "FLDGPAK1" | u32 version(1) | u32 entryCount | u64 tocOffset | u64 tocSize
//   Blobs        : entry data, each stored raw or Fledge-LZ compressed
//   TOC          : per entry: u16 pathLen | path (UTF-8, '/'-separated) | u64 offset | u64 storedSize
//                  | u64 originalSize | u32 crc32(original) | u8 compression (0=none, 1=lz)
//
// Appended to an executable, the pak is followed by a 32-byte footer:
//   "FLDGFTR1" | u64 pakOffset | u64 pakSize | u32 version(1) | u32 reserved
// so the runtime finds its content by reading the last 32 bytes of itself.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "fledge/core/utility.h"
#include "fledge/core/vfs.h"

namespace fledge {

enum class PakCompression : uint8_t { None = 0, Lz = 1 };

struct PakEntryInfo {
    std::string path;
    uint64_t offset = 0;
    uint64_t storedSize = 0;
    uint64_t originalSize = 0;
    uint32_t crc = 0;
    PakCompression compression = PakCompression::None;
};

class PakWriter {
public:
    void add(std::string path, Bytes data);
    void setCompression(bool enabled) { m_compress = enabled; }
    // progress(done, total, path) while entries are compressed. Ordering of the pak does not change.
    Bytes build(const std::function<void(size_t done, size_t total, const std::string& path)>& progress = {}) const;
    bool writeTo(const fs::path& file) const;
    size_t entryCount() const { return m_entries.size(); }

private:
    std::map<std::string, Bytes> m_entries;  // sorted, deterministic output
    bool m_compress = true;
};

class PakReader {
public:
    static std::unique_ptr<PakReader> openMemory(Bytes image);
    static std::unique_ptr<PakReader> openFile(const fs::path& file, uint64_t offset = 0, uint64_t size = 0);
    // Opens a pak appended to an executable (footer at end of file). nullptr if none.
    static std::unique_ptr<PakReader> openAppended(const fs::path& exe);

    std::optional<Bytes> read(std::string_view path) const;  // verifies CRC
    bool contains(std::string_view path) const { return m_index.contains(std::string(path)); }
    std::vector<std::string> list() const;
    const std::map<std::string, PakEntryInfo, std::less<>>& entries() const { return m_index; }

private:
    bool parse(std::span<const uint8_t> headerAndToc, uint64_t pakSize);
    std::optional<Bytes> readRange(uint64_t offset, uint64_t size) const;

    Bytes m_memory;           // memory mode
    fs::path m_file;          // file mode
    uint64_t m_base = 0;      // file offset of the pak start
    uint64_t m_size = 0;
    std::map<std::string, PakEntryInfo, std::less<>> m_index;
    mutable std::mutex m_mutex;
};

// VFS adapter so a pak can be mounted as "content:/".
class PakProvider final : public FileProvider {
public:
    explicit PakProvider(std::shared_ptr<PakReader> pak) : m_pak(std::move(pak)) {}
    std::optional<Bytes> read(std::string_view p) const override { return m_pak->read(p); }
    bool exists(std::string_view p) const override { return m_pak->contains(p); }
    std::vector<std::string> list() const override { return m_pak->list(); }

private:
    std::shared_ptr<PakReader> m_pak;
};

constexpr char kPakMagic[8] = {'C', 'A', 'G', 'E', 'P', 'A', 'K', '1'};
constexpr char kFooterMagic[8] = {'C', 'A', 'G', 'E', 'F', 'T', 'R', '1'};
constexpr size_t kPakHeaderSize = 32;
constexpr size_t kPakFooterSize = 32;

// Builds the 32-byte footer that follows an appended pak.
Bytes makePakFooter(uint64_t pakOffset, uint64_t pakSize);

} // namespace fledge
