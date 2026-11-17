#include "fledge/assets/fpack.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

#include "fledge/assets/compression.h"
#include "fledge/assets/cooked.h"  // contentHash (FNV-1a 64)
#include "fledge/core/jobs.h"
#include "fledge/core/log.h"

namespace fledge {

namespace {
void put16(Bytes& b, uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }

struct Cursor {
    std::span<const uint8_t> d;
    size_t pos = 0;
    bool ok = true;
    template <typename T>
    T get() {
        if (pos + sizeof(T) > d.size()) { ok = false; return T{}; }
        // Widening each byte to T first keeps the <<56 case defined for u64.
        T v = 0;
        for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(d[pos + i]) << (8 * i);
        pos += sizeof(T);
        return v;
    }
    std::string str(size_t n) {
        if (pos + n > d.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(d.data() + pos), n);
        pos += n;
        return s;
    }
};

bool validEntryPath(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.back() == '/' || p.find('\\') != std::string::npos) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        std::string_view seg(p.data() + start, (slash == std::string::npos ? p.size() : slash) - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

} // namespace

Bytes serializeFpackIndex(const FpackIndex& idx) {
    Bytes out;
    out.reserve(idx.entries.size() * 48 + idx.chunks.size() * 32);
    for (const FpackEntry& e : idx.entries) {
        put16(out, static_cast<uint16_t>(e.path.size()));
        out.insert(out.end(), e.path.begin(), e.path.end());
        put64(out, e.rawSize);
        put32(out, static_cast<uint32_t>(e.chunks.size()));
        put32(out, e.crc);
        for (uint64_t ci : e.chunks) put64(out, ci);
    }
    for (const FpackChunk& c : idx.chunks) {
        put64(out, c.hash);
        put64(out, c.offset);
        put32(out, c.storedSize);
        put32(out, c.rawSize);
        out.push_back(static_cast<uint8_t>(c.codec));
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        put32(out, c.crc);
    }
    return out;
}

bool parseIndex(std::span<const uint8_t> data, uint32_t entryCount, uint32_t chunkCount, uint32_t chunkShift, uint64_t pakSize,
                std::vector<FpackEntry>& entriesOut, std::vector<FpackChunk>& chunksOut) {
    const uint64_t chunkSize = 1ull << chunkShift;
    // Chunk table bounds-check every payload before any entry is trusted.
    if (chunkCount > 0 && chunkCount > pakSize) return false;  // absurd count for the size
    entriesOut.clear();
    chunksOut.clear();
    entriesOut.reserve(entryCount);
    chunksOut.reserve(chunkCount);
    Cursor c{data};
    // Entries first (they reference chunks by index; validated after both tables are in).
    struct RawEntry {
        std::string path;
        uint64_t rawSize;
        uint32_t crc;
        std::vector<uint64_t> chunks;
    };
    std::vector<RawEntry> raw;
    raw.reserve(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i) {
        RawEntry e;
        auto len = c.get<uint16_t>();
        if (len == 0) return false;
        e.path = c.str(len);
        e.rawSize = c.get<uint64_t>();
        uint32_t n = c.get<uint32_t>();
        e.crc = c.get<uint32_t>();
        uint64_t maxChunks = (e.rawSize + chunkSize - 1) / chunkSize;
        if (n != maxChunks) return false;  // exactly ceil(size/chunk) chunks, 0 for an empty entry
        if (!c.ok) return false;
        e.chunks.reserve(n);
        for (uint32_t k = 0; k < n; ++k) e.chunks.push_back(c.get<uint64_t>());
        if (!c.ok || !validEntryPath(e.path)) return false;
        raw.push_back(std::move(e));
    }
    for (uint32_t i = 0; i < chunkCount; ++i) {
        FpackChunk ch;
        ch.hash = c.get<uint64_t>();
        ch.offset = c.get<uint64_t>();
        ch.storedSize = c.get<uint32_t>();
        ch.rawSize = c.get<uint32_t>();
        ch.codec = static_cast<FpackCodec>(c.get<uint8_t>());
        c.get<uint8_t>();
        c.get<uint8_t>();
        c.get<uint8_t>();
        ch.crc = c.get<uint32_t>();
        if (!c.ok) return false;
        if (ch.codec != FpackCodec::Raw && ch.codec != FpackCodec::Lz) return false;
        if (ch.rawSize == 0 || ch.rawSize > chunkSize) return false;
        if (ch.offset < kFpackHeaderSize || ch.offset > pakSize || ch.storedSize > pakSize - ch.offset) return false;
        chunksOut.push_back(ch);
    }
    for (RawEntry& e : raw) {
        uint64_t sum = 0;
        for (uint64_t ci : e.chunks) {
            if (ci >= chunksOut.size()) return false;
            sum += chunksOut[static_cast<size_t>(ci)].rawSize;
        }
        if (sum != e.rawSize) return false;
        FpackEntry fe;
        fe.path = std::move(e.path);
        fe.rawSize = e.rawSize;
        fe.crc = e.crc;
        fe.chunks = std::move(e.chunks);
        entriesOut.push_back(std::move(fe));
    }
    return true;
}

// ---------------------------------------------------------------- chunks ---
FpackCodec encodeChunk(std::span<const uint8_t> raw, Bytes& storedOut) {
    if (raw.size() >= 64) {
        Bytes packed = lz::compress(raw);
        if (!packed.empty() && packed.size() <= raw.size() * 97 / 100 && raw.size() - packed.size() >= 16) {
            storedOut = std::move(packed);
            return FpackCodec::Lz;
        }
    }
    storedOut.assign(raw.begin(), raw.end());
    return FpackCodec::Raw;
}

// ---------------------------------------------------------------- writer ---
void FpackWriter::add(std::string path, Bytes data) { m_entries[std::move(path)] = std::move(data); }

void FpackWriter::setChunks(const std::vector<FpackChunk>& chunks, const std::unordered_map<uint64_t, Bytes>& storedByHash) {
    m_shared.clear();
    for (const FpackChunk& c : chunks) {
        auto it = storedByHash.find(c.hash);
        if (it != storedByHash.end() && it->second.size() == c.storedSize) m_shared.emplace(c.hash, it->second);
    }
}

Result<FpackIndex> FpackWriter::buildIndex(std::ostream& out,
                                           const std::function<void(size_t, size_t, const std::string&)>& progress) const {
    FpackIndex idx;
    std::unordered_map<uint64_t, uint64_t> emitted;  // hash -> chunk table index
    Bytes header(kFpackHeaderSize, 0);
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    uint64_t offset = kFpackHeaderSize;
    const size_t n = m_entries.size();
    size_t done = 0;
    for (const auto& [path, data] : m_entries) {
        if (!validEntryPath(path)) return Result<FpackIndex>::failure("invalid pak entry path '" + path + "'");
        FpackEntry e;
        e.path = path;
        e.rawSize = data.size();
        e.crc = crc32(data);
        uint64_t pos = 0;
        while (pos < data.size()) {
            const size_t len = static_cast<size_t>(std::min<uint64_t>(1ull << kFpackChunkShift, data.size() - pos));
            std::span<const uint8_t> rawChunk(data.data() + pos, len);
            const uint64_t hash = contentHash(rawChunk);
            auto it = emitted.find(hash);
            if (it == emitted.end()) {
                FpackChunk ch;
                ch.hash = hash;
                ch.offset = offset;
                ch.rawSize = static_cast<uint32_t>(len);
                ch.crc = crc32(rawChunk);
                Bytes stored;
                auto sharedIt = m_shared.find(hash);
                if (sharedIt != m_shared.end()) {
                    // Unchanged chunk from the previous archive: reuse the payload
                    // verbatim and skip re-compression. The codec flag recorded in the
                    // previous table does not survive setChunks (payload-only map), so
                    // recover it by trial-decompressing; LZ is self-terminating, so a
                    // decoded size != raw size means the payload is raw.
                    stored = sharedIt->second;
                    if (auto dec = lz::decompress(stored, len); dec && dec->size() == len)
                        ch.codec = FpackCodec::Lz;
                    else if (stored.size() == len)
                        ch.codec = FpackCodec::Raw;
                    else {
                        // Payload inconsistent with the recorded size; re-encode.
                        ch.codec = encodeChunk(rawChunk, stored);
                    }
                } else {
                    ch.codec = encodeChunk(rawChunk, stored);
                }
                ch.storedSize = static_cast<uint32_t>(stored.size());
                out.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
                if (!out) return Result<FpackIndex>::failure("failed while streaming pak chunks");
                offset += stored.size();
                it = emitted.emplace(hash, idx.chunks.size()).first;
                idx.chunks.push_back(std::move(ch));
            }
            e.chunks.push_back(it->second);
            pos += len;
        }
        idx.entries.push_back(std::move(e));
        ++done;
        if (progress && (done == n || done % 32 == 0)) progress(done, n, path);
    }
    if (!out.good()) return Result<FpackIndex>::failure("failed while streaming pak chunks");
    return idx;
}

Result<Bytes> FpackWriter::build(const std::function<void(size_t, size_t, const std::string&)>& progress) const {
    std::ostringstream out(std::ios::binary);
    auto idx = buildIndex(out, progress);
    if (!idx) return Result<Bytes>::failure(idx.error());
    // buildIndex streams a 48-byte placeholder header followed by the chunk
    // payloads; replace the placeholder with the real header.
    const std::string chunkBlob = out.str();
    Bytes index = serializeFpackIndex(*idx);
    Bytes header;
    header.insert(header.end(), kFpackMagic, kFpackMagic + 8);
    put32(header, kFpackVersion);
    put32(header, 0);  // flags
    put32(header, kFpackChunkShift);
    put32(header, 0);  // reserved
    put64(header, idx->entries.size());
    put64(header, idx->chunks.size());
    put64(header, chunkBlob.size());  // indexOffset: chunkBlob already includes the header
    put64(header, index.size());
    Bytes image;
    image.reserve(chunkBlob.size() + index.size());
    image.insert(image.end(), header.begin(), header.end());
    image.insert(image.end(), chunkBlob.begin() + kFpackHeaderSize, chunkBlob.end());
    image.insert(image.end(), index.begin(), index.end());
    return image;
}

bool FpackWriter::writeTo(const fs::path& file) const {
    auto img = build();
    if (!img) return false;
    return writeFileBytes(file, *img);
}

Bytes makeFpackFooter(uint64_t pakOffset, uint64_t pakSize) {
    Bytes f(8, 0);
    std::memcpy(f.data(), "CAGEFTR1", 8);
    put64(f, pakOffset);
    put64(f, pakSize);
    put32(f, kFpackVersion);
    put32(f, 0);
    return f;
}

std::optional<FpackIndex> readFpackIndex(const fs::path& file) {
    // Accept both a bare .fpak and an exe with an appended v2 pak.
    auto r = FpackReader::openFile(file);
    if (!r) r = FpackReader::openAppended(file);
    if (!r) return std::nullopt;
    FpackIndex idx;
    idx.chunkShift = r->chunkShift();
    idx.chunks = r->chunks();
    idx.entries.reserve(r->entries().size());
    for (const auto& [path, e] : r->entries()) idx.entries.push_back(e);
    return idx;
}

// ---------------------------------------------------------------- reader ---
bool FpackReader::parse(std::span<const uint8_t> hdr, uint64_t pakSize) {
    if (hdr.size() < kFpackHeaderSize || std::memcmp(hdr.data(), kFpackMagic, 8) != 0) return false;
    Cursor h{hdr.subspan(8, kFpackHeaderSize - 8)};
    uint32_t version = h.get<uint32_t>();
    h.get<uint32_t>();  // flags
    uint32_t chunkShift = h.get<uint32_t>();
    h.get<uint32_t>();  // reserved
    uint64_t entryCount = h.get<uint64_t>();
    uint64_t chunkCount = h.get<uint64_t>();
    uint64_t indexOffset = h.get<uint64_t>();
    uint64_t indexSize = h.get<uint64_t>();
    if (version != kFpackVersion) return false;
    if (chunkShift < 8 || chunkShift > 24) return false;  // 256 B .. 16 MiB chunks
    if (entryCount > (1u << 24) || chunkCount > (1ull << 32)) return false;
    if (indexOffset > pakSize || indexSize > pakSize - indexOffset) return false;
    // Bounds are computed against the real archive size (in memory mode the
    // 'hdr' span only holds the 48-byte header, so use the full image size).
    const uint64_t realSize = m_memory.empty() ? pakSize : m_memory.size();
    if (indexOffset > realSize || indexSize > realSize - indexOffset) return false;
    auto indexData = readRange(indexOffset, indexSize);
    if (!indexData) return false;
    std::vector<FpackEntry> entries;
    std::vector<FpackChunk> chunks;
    if (!parseIndex(*indexData, static_cast<uint32_t>(entryCount), static_cast<uint32_t>(chunkCount), chunkShift, realSize, entries,
                    chunks))
        return false;
    m_chunkShift = chunkShift;
    m_chunks = std::move(chunks);
    for (auto& e : entries) m_index[e.path] = std::move(e);
    m_size = realSize;
    return true;
}

std::optional<Bytes> FpackReader::readRange(uint64_t offset, uint64_t size) const {
    if (!m_memory.empty()) {
        if (offset + size > m_memory.size()) return std::nullopt;
        return Bytes(m_memory.begin() + static_cast<std::ptrdiff_t>(offset),
                     m_memory.begin() + static_cast<std::ptrdiff_t>(offset + size));
    }
    std::lock_guard lock(m_mutex);
    std::ifstream f(osPath(m_file), std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(static_cast<std::streamoff>(m_base + offset));
    Bytes b(static_cast<size_t>(size));
    if (size && !f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return b;
}

std::unique_ptr<FpackReader> FpackReader::openMemory(Bytes image) {
    auto r = std::unique_ptr<FpackReader>(new FpackReader());
    r->m_memory = std::move(image);
    Bytes header(r->m_memory.begin(),
                 r->m_memory.begin() + std::min<std::ptrdiff_t>(kFpackHeaderSize, static_cast<std::ptrdiff_t>(r->m_memory.size())));
    if (!r->parse(header, r->m_memory.size())) return nullptr;
    return r;
}

std::unique_ptr<FpackReader> FpackReader::openFile(const fs::path& file, uint64_t offset, uint64_t size) {
    std::error_code ec;
    uint64_t fileSize = fs::file_size(osPath(file), ec);
    if (ec || offset > fileSize) return nullptr;
    if (size == 0) size = fileSize - offset;
    if (size > fileSize - offset) return nullptr;
    auto r = std::unique_ptr<FpackReader>(new FpackReader());
    r->m_file = file;
    r->m_base = offset;
    auto header = r->readRange(0, kFpackHeaderSize);
    if (!header || !r->parse(*header, size)) return nullptr;
    return r;
}

std::unique_ptr<FpackReader> FpackReader::openAppended(const fs::path& exe) {
    std::error_code ec;
    uint64_t fileSize = fs::file_size(osPath(exe), ec);
    if (ec || fileSize < 32) return nullptr;
    std::ifstream f(osPath(exe), std::ios::binary);
    f.seekg(static_cast<std::streamoff>(fileSize - 32));
    uint8_t buf[32];
    if (!f.read(reinterpret_cast<char*>(buf), 32)) return nullptr;
    if (std::memcmp(buf, "CAGEFTR1", 8) != 0) return nullptr;
    Cursor c{std::span<const uint8_t>(buf + 8, 24)};
    uint64_t off = c.get<uint64_t>();
    uint64_t size = c.get<uint64_t>();
    uint32_t version = c.get<uint32_t>();
    if (version != kFpackVersion) return nullptr;  // v1 payload: PakReader handles it
    if (off > fileSize || size > fileSize - off || size + 32 > fileSize - off) return nullptr;
    return openFile(exe, off, size);
}

std::optional<Bytes> FpackReader::read(std::string_view path) const {
    auto it = m_index.find(path);
    if (it == m_index.end()) return std::nullopt;
    const FpackEntry& e = it->second;
    Bytes out;
    out.reserve(static_cast<size_t>(e.rawSize));
    for (uint64_t ci : e.chunks) {
        const FpackChunk& ch = m_chunks[static_cast<size_t>(ci)];
        auto stored = readRange(ch.offset, ch.storedSize);
        if (!stored) return std::nullopt;
        std::optional<Bytes> raw;
        if (ch.codec == FpackCodec::Lz) raw = lz::decompress(*stored, ch.rawSize);
        else if (stored->size() == ch.rawSize) raw = std::move(stored);
        if (!raw || raw->size() != ch.rawSize || crc32(*raw) != ch.crc) {
            FLEDGE_LOG_ERROR("fpack", "Corrupt chunk in pak entry '{}'", e.path);
            return std::nullopt;
        }
        out.insert(out.end(), raw->begin(), raw->end());
    }
    if (out.size() != e.rawSize || crc32(out) != e.crc) {
        FLEDGE_LOG_ERROR("fpack", "Corrupt pak entry '{}'", e.path);
        return std::nullopt;
    }
    return out;
}

std::vector<std::string> FpackReader::list() const {
    std::vector<std::string> out;
    for (auto& [p, e] : m_index) out.push_back(p);
    return out;
}

} // namespace fledge
