#include "fledge/assets/pak.h"

#include <atomic>
#include <cstring>
#include <fstream>

#include "fledge/assets/compression.h"
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
} // namespace

// ----------------------------------------------------------------- writer --
void PakWriter::add(std::string path, Bytes data) { m_entries[std::move(path)] = std::move(data); }

namespace {
// LZ on a PNG, JPEG, OGG or a BC texture spends the time and almost never shrinks the file.
bool alreadyCompressed(const std::string& path, const Bytes& data) {
    std::string l = toLower(path);
    for (const char* ext : {".png", ".jpg", ".jpeg", ".webp", ".ogg", ".mp3", ".flac", ".opus", ".dds", ".ktx", ".ktx2"})
        if (endsWith(l, ext)) return true;
    if (endsWith(l, ".ctex") && data.size() >= 12 && data[0] == 'C' && data[1] == 'T' && data[2] == 'E' && data[3] == 'X') {
        uint32_t format = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8) | (static_cast<uint32_t>(data[10]) << 16) |
                          (static_cast<uint32_t>(data[11]) << 24);
        if (format == 1 || format == 2) return true;  // BC1 / BC3
    }
    return false;
}
} // namespace

Bytes PakWriter::build(const std::function<void(size_t, size_t, const std::string&)>& progress) const {
    struct Item {
        std::string path;
        const Bytes* data = nullptr;
        Bytes stored;
        uint32_t crc = 0;
        bool compressed = false;
    };
    std::vector<Item> items;
    items.reserve(m_entries.size());
    for (const auto& [path, data] : m_entries) items.push_back({path, &data, {}, 0, false});
    const size_t n = items.size();
    std::atomic<size_t> finished{0};
    auto one = [&](size_t i) {
        Item& e = items[i];
        e.crc = crc32(*e.data);
        if (m_compress && e.data->size() > 64 && !alreadyCompressed(e.path, *e.data)) {
            Bytes packed = lz::compress(*e.data);
            if (!packed.empty() && packed.size() < e.data->size() * 95 / 100) {
                e.compressed = true;
                e.stored = std::move(packed);
            }
        }
        size_t done = finished.fetch_add(1) + 1;
        if (progress && (done == n || done % 32 == 0)) progress(done, n, e.path);
    };
    if (progress && n > 0) progress(0, n, items.front().path);
    if (n > 1) {
        JobSystem jobs;
        jobs.parallelFor(n, one);
    } else if (n == 1) {
        one(0);
    }
    uint64_t payload = 0;
    for (const Item& e : items) payload += e.compressed ? e.stored.size() : e.data->size();
    Bytes out;
    out.reserve(static_cast<size_t>(kPakHeaderSize + payload + n * 48));
    out.assign(kPakHeaderSize, 0);
    std::vector<PakEntryInfo> infos;
    infos.reserve(n);
    for (Item& e : items) {
        PakEntryInfo info;
        info.path = e.path;
        info.originalSize = e.data->size();
        info.crc = e.crc;
        info.offset = out.size();
        const Bytes& blob = e.compressed ? e.stored : *e.data;
        info.compression = e.compressed ? PakCompression::Lz : PakCompression::None;
        info.storedSize = blob.size();
        out.insert(out.end(), blob.begin(), blob.end());
        infos.push_back(std::move(info));
    }
    uint64_t tocOffset = out.size();
    for (const auto& e : infos) {
        put16(out, static_cast<uint16_t>(e.path.size()));
        out.insert(out.end(), e.path.begin(), e.path.end());
        put64(out, e.offset);
        put64(out, e.storedSize);
        put64(out, e.originalSize);
        put32(out, e.crc);
        out.push_back(static_cast<uint8_t>(e.compression));
    }
    uint64_t tocSize = out.size() - tocOffset;
    Bytes header;
    header.insert(header.end(), kPakMagic, kPakMagic + 8);
    put32(header, 1);
    put32(header, static_cast<uint32_t>(infos.size()));
    put64(header, tocOffset);
    put64(header, tocSize);
    std::memcpy(out.data(), header.data(), kPakHeaderSize);
    return out;
}

bool PakWriter::writeTo(const fs::path& file) const { return writeFileBytes(file, build()); }

Bytes makePakFooter(uint64_t pakOffset, uint64_t pakSize) {
    Bytes f(kFooterMagic, kFooterMagic + 8);
    put64(f, pakOffset);
    put64(f, pakSize);
    put32(f, 1);
    put32(f, 0);
    return f;
}

// ----------------------------------------------------------------- reader --
bool PakReader::parse(std::span<const uint8_t> img, uint64_t pakSize) {
    // img must start with the header; TOC is located via readRange in file mode.
    if (img.size() < kPakHeaderSize || std::memcmp(img.data(), kPakMagic, 8) != 0) return false;
    Cursor h{img.subspan(8, 24)};
    uint32_t version = h.get<uint32_t>();
    uint32_t count = h.get<uint32_t>();
    uint64_t tocOffset = h.get<uint64_t>();
    uint64_t tocSize = h.get<uint64_t>();
    if (version != 1 || tocOffset + tocSize > pakSize) return false;
    auto toc = readRange(tocOffset, tocSize);
    if (!toc) return false;
    Cursor c{*toc};
    for (uint32_t i = 0; i < count; ++i) {
        PakEntryInfo e;
        auto len = c.get<uint16_t>();
        e.path = c.str(len);
        e.offset = c.get<uint64_t>();
        e.storedSize = c.get<uint64_t>();
        e.originalSize = c.get<uint64_t>();
        e.crc = c.get<uint32_t>();
        e.compression = static_cast<PakCompression>(c.get<uint8_t>());
        if (!c.ok || e.offset + e.storedSize > pakSize) return false;
        m_index[e.path] = std::move(e);
    }
    m_size = pakSize;
    return true;
}

std::optional<Bytes> PakReader::readRange(uint64_t offset, uint64_t size) const {
    if (!m_memory.empty()) {
        if (offset + size > m_memory.size()) return std::nullopt;
        return Bytes(m_memory.begin() + static_cast<std::ptrdiff_t>(offset),
                     m_memory.begin() + static_cast<std::ptrdiff_t>(offset + size));
    }
    std::lock_guard lock(m_mutex);
    std::ifstream f(osPath(m_file), std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(static_cast<std::streamoff>(m_base + offset));
    Bytes b(size);
    if (size && !f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return b;
}

std::unique_ptr<PakReader> PakReader::openMemory(Bytes image) {
    auto r = std::unique_ptr<PakReader>(new PakReader());
    r->m_memory = std::move(image);
    Bytes header(r->m_memory.begin(), r->m_memory.begin() + std::min<std::ptrdiff_t>(kPakHeaderSize, static_cast<std::ptrdiff_t>(r->m_memory.size())));
    if (!r->parse(header, r->m_memory.size())) return nullptr;
    return r;
}

std::unique_ptr<PakReader> PakReader::openFile(const fs::path& file, uint64_t offset, uint64_t size) {
    std::error_code ec;
    uint64_t fileSize = fs::file_size(osPath(file), ec);
    if (ec) return nullptr;
    if (size == 0) size = fileSize - offset;
    if (offset + size > fileSize) return nullptr;
    auto r = std::unique_ptr<PakReader>(new PakReader());
    r->m_file = file;
    r->m_base = offset;
    auto header = r->readRange(0, kPakHeaderSize);
    if (!header || !r->parse(*header, size)) return nullptr;
    return r;
}

std::unique_ptr<PakReader> PakReader::openAppended(const fs::path& exe) {
    std::error_code ec;
    uint64_t fileSize = fs::file_size(osPath(exe), ec);
    if (ec || fileSize < kPakFooterSize) return nullptr;
    std::ifstream f(osPath(exe), std::ios::binary);
    f.seekg(static_cast<std::streamoff>(fileSize - kPakFooterSize));
    uint8_t buf[kPakFooterSize];
    if (!f.read(reinterpret_cast<char*>(buf), kPakFooterSize)) return nullptr;
    if (std::memcmp(buf, kFooterMagic, 8) != 0) return nullptr;
    Cursor c{std::span<const uint8_t>(buf + 8, 24)};
    uint64_t off = c.get<uint64_t>();
    uint64_t size = c.get<uint64_t>();
    if (off + size + kPakFooterSize > fileSize) return nullptr;
    return openFile(exe, off, size);
}

std::optional<Bytes> PakReader::read(std::string_view path) const {
    auto it = m_index.find(path);
    if (it == m_index.end()) return std::nullopt;
    const auto& e = it->second;
    auto stored = readRange(e.offset, e.storedSize);
    if (!stored) return std::nullopt;
    std::optional<Bytes> data;
    if (e.compression == PakCompression::Lz) data = lz::decompress(*stored, e.originalSize);
    else data = std::move(stored);
    if (!data || crc32(*data) != e.crc) {
        FLEDGE_LOG_ERROR("pak", "Corrupt pak entry '{}'", e.path);
        return std::nullopt;
    }
    return data;
}

std::vector<std::string> PakReader::list() const {
    std::vector<std::string> out;
    for (auto& [p, e] : m_index) out.push_back(p);
    return out;
}

} // namespace fledge
