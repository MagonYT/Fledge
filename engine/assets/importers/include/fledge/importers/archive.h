#pragma once
// Safe archive readers for store downloads (editor/tools only): raw DEFLATE, gzip, ustar/GNU/pax tar, zip.
//
// Everything here treats its input as hostile:
//   * entry paths go through sanitizeEntryPath() (no "..", absolute, drive, UNC, ADS, device names, control chars);
//   * symlinks, hard links and device entries are never created - they are skipped and reported;
//   * Limits bound the archive size, every entry, the total output, the number of entries and the compression
//     ratio (decompression bombs), and the decoders stop as soon as one is exceeded;
//   * nothing extracted is ever loaded as code or executed.
// Decoding streams with bounded memory (a 32 KiB window), so multi-GB .unitypackage files do not need RAM.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge::archive {

struct Limits {
    uint64_t maxArchiveBytes = 16ull << 30;  // compressed input
    uint64_t maxEntryBytes = 4ull << 30;     // one extracted file
    uint64_t maxTotalBytes = 16ull << 30;    // all extracted bytes together
    uint32_t maxEntries = 200000;
    double maxRatio = 250.0;                 // total output / compressed input read so far (after the first 4 MiB)
    size_t maxPathLength = 1024;
    size_t maxPathDepth = 64;
};

// A '/'-separated relative path that is safe to create below a destination folder, or nullopt.
// Backslashes count as separators, "./" components are dropped; "..", absolute paths, drive letters, UNC prefixes,
// ':' (alternate data streams), control characters, reserved device names (CON, NUL, COM1 ...) and components ending
// in a dot or space are rejected.
std::optional<std::string> sanitizeEntryPath(std::string_view raw, const Limits& limits = {});
// True when `p` (after resolving "..", symlinks and junctions that exist) lies inside `root`. Importers use it before
// reading a file that a model or package refers to, so "../../secret" references cannot pull in unrelated files.
bool pathWithin(const fs::path& root, const fs::path& p);

// Pull source of bytes (file or memory).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual size_t read(uint8_t* dst, size_t n) = 0;  // 0 = end of input
    uint64_t consumed = 0;                            // bytes handed out so far
};
class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::span<const uint8_t> data) : m_data(data) {}
    size_t read(uint8_t* dst, size_t n) override;
private:
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
};
class FileSource final : public ByteSource {
public:
    explicit FileSource(const fs::path& file);
    ~FileSource() override;
    bool ok() const { return m_file != nullptr; }
    uint64_t size() const { return m_size; }
    bool seek(uint64_t offset);
    size_t read(uint8_t* dst, size_t n) override;
private:
    std::FILE* m_file = nullptr;
    uint64_t m_size = 0;
};

// Receives decoded bytes; return false to abort (the decoder then fails with the sink's message).
using Sink = std::function<bool(const uint8_t* data, size_t size)>;

// Raw DEFLATE (RFC 1951) from `in` (at most maxInput bytes) into `out`. maxOutput bounds the output.
Status inflateRaw(ByteSource& in, const Sink& out, uint64_t maxOutput, uint64_t maxInput = ~0ull, uint32_t* crcOut = nullptr);
// gzip (RFC 1952, multi-member) -> out; checks CRC-32 and size, and the ratio / size limits.
Status gunzip(ByteSource& in, const Sink& out, const Limits& limits);
// Whole-buffer helpers (tests, small payloads).
Result<Bytes> gunzipBuffer(std::span<const uint8_t> data, const Limits& limits = {});
Result<Bytes> inflateBuffer(std::span<const uint8_t> raw, uint64_t maxOutput);

// ---- tar (ustar, GNU long names, pax path/size); fed by push (e.g. from gunzip).
struct TarEntry {
    std::string path;      // sanitised; empty when rejected (see skipReason)
    std::string rawPath;   // as stored
    uint64_t size = 0;
    char type = '0';       // '0' file, '5' directory, '1'/'2' links, others
    std::string skipReason;  // non-empty: the entry is not extracted (link, device, unsafe path ...)
};
class TarParser {
public:
    // onEntry: called for every header; return a data sink for regular files to receive the bytes, or {} to skip.
    // onEnd: called after a regular file's last byte.
    using OnEntry = std::function<Sink(const TarEntry&)>;
    TarParser(Limits limits, OnEntry onEntry, std::function<void(const TarEntry&)> onEnd = {});
    bool feed(const uint8_t* data, size_t size);  // false on error (see error())
    Status finish() const;                         // the archive ended cleanly
    const std::string& error() const { return m_error; }
    const std::vector<std::string>& warnings() const { return m_warnings; }
    uint32_t entries() const { return m_entries; }
    uint64_t totalBytes() const { return m_total; }
private:
    bool header();
    bool complete();  // the current entry's data / extended header has been read
    Limits m_limits;
    OnEntry m_onEntry;
    std::function<void(const TarEntry&)> m_onEnd;
    uint8_t m_block[512];
    size_t m_blockFill = 0;
    enum class State { Header, Data, LongName, Pax, Done } m_state = State::Header;
    TarEntry m_cur;
    Sink m_sink;
    uint64_t m_remaining = 0, m_padding = 0;
    std::string m_meta;                   // GNU long name / pax record being collected
    std::string m_nextPath, m_globalPath; // from 'L' / 'x'
    std::optional<uint64_t> m_nextSize;
    int m_zeroBlocks = 0;
    uint32_t m_entries = 0;
    uint64_t m_total = 0;
    std::string m_error;
    std::vector<std::string> m_warnings;
};

// ---- zip (stored / deflate, ZIP64); random access over a file.
struct ZipEntry {
    std::string path;        // sanitised; empty when rejected
    std::string rawPath;
    uint64_t compressedSize = 0, size = 0, localHeader = 0;
    uint16_t method = 0, flags = 0;
    uint32_t crc = 0;
    bool directory = false;
    std::string skipReason;  // symlink, encrypted, unsupported method, unsafe path ...
};
class ZipReader {
public:
    Status open(const fs::path& file, const Limits& limits = {});
    const std::vector<ZipEntry>& entries() const { return m_entries; }
    // Decodes one entry into out (size, CRC and limits checked).
    Status extract(const ZipEntry& e, const Sink& out);
    // Extracts every accepted entry below dest; warnings list what was skipped. Returns the written relative paths.
    Result<std::vector<std::string>> extractAll(const fs::path& dest, std::vector<std::string>& warnings);
private:
    std::unique_ptr<FileSource> m_src;
    std::vector<ZipEntry> m_entries;
    Limits m_limits;
    uint64_t m_written = 0;
};

// Writes a stream to a file below `dest` (created on demand); used as a Sink by extractors.
class FileSink {
public:
    explicit FileSink(const fs::path& file);
    ~FileSink();
    bool ok() const { return m_file != nullptr; }
    bool write(const uint8_t* data, size_t size);
    void close();
private:
    std::FILE* m_file = nullptr;
};

} // namespace fledge::archive
