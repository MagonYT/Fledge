// Safe archive readers: streaming raw DEFLATE / gzip, a push tar parser and a zip reader (see archive.h).
#include "fledge/importers/archive.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <format>
#include <set>

namespace fledge::archive {

// ------------------------------------------------------------------ paths ----
std::optional<std::string> sanitizeEntryPath(std::string_view raw, const Limits& limits) {
    if (raw.empty() || raw.size() > limits.maxPathLength) return std::nullopt;
    std::string p(raw);
    for (char& c : p)
        if (c == '\\') c = '/';
    if (p[0] == '/') return std::nullopt;                                   // absolute (also //server UNC)
    if (p.size() >= 2 && p[1] == ':') return std::nullopt;                  // drive letter
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= p.size()) {
        size_t e = p.find('/', start);
        if (e == std::string::npos) e = p.size();
        std::string part = p.substr(start, e - start);
        start = e + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return std::nullopt;
        for (unsigned char c : part)
            if (c < 0x20 || c == 0x7F || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return std::nullopt;
        if (part.back() == '.' || part.back() == ' ') return std::nullopt;  // Windows strips them: aliases another name
        std::string stem = toLower(part.substr(0, part.find('.')));
        static const std::set<std::string> kReserved = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                                        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9", "conin$", "conout$"};
        if (kReserved.contains(stem)) return std::nullopt;
        parts.push_back(std::move(part));
    }
    if (parts.empty() || parts.size() > limits.maxPathDepth) return std::nullopt;
    std::string out;
    for (auto& s : parts) out += (out.empty() ? "" : "/") + s;
    return out;
}

bool pathWithin(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::weakly_canonical(osPath(root), ec);
    if (ec) return false;
    fs::path q = fs::weakly_canonical(osPath(p), ec);
    if (ec) return false;
    std::wstring rs = r.wstring(), qs = q.wstring();
    auto lower = [](std::wstring s) {  // Windows paths compare case-insensitively; "\\?\" prefixes are dropped
        for (auto& c : s) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
        if (s.rfind(L"\\\\?\\unc\\", 0) == 0) s = L"\\\\" + s.substr(8);
        else if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
        return s;
    };
    rs = lower(rs);
    qs = lower(qs);
    while (!rs.empty() && rs.back() == L'\\') rs.pop_back();
    return !rs.empty() && qs.size() > rs.size() && qs.compare(0, rs.size(), rs) == 0 && qs[rs.size()] == L'\\';
}

// ---------------------------------------------------------------- sources ----
size_t MemorySource::read(uint8_t* dst, size_t n) {
    n = std::min(n, m_data.size() - m_pos);
    if (n) std::memcpy(dst, m_data.data() + m_pos, n);
    m_pos += n;
    consumed += n;
    return n;
}

FileSource::FileSource(const fs::path& file) {
#ifdef _WIN32
    m_file = _wfopen(osPath(file).c_str(), L"rb");
#else
    m_file = std::fopen(file.c_str(), "rb");
#endif
    std::error_code ec;
    if (m_file) m_size = fs::file_size(osPath(file), ec);
}
FileSource::~FileSource() {
    if (m_file) std::fclose(m_file);
}
bool FileSource::seek(uint64_t offset) {
#ifdef _WIN32
    return m_file && _fseeki64(m_file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return m_file && fseeko(m_file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
size_t FileSource::read(uint8_t* dst, size_t n) {
    if (!m_file) return 0;
    size_t got = std::fread(dst, 1, n, m_file);
    consumed += got;
    return got;
}

FileSink::FileSink(const fs::path& file) {
    std::error_code ec;
    fs::create_directories(osPath(file.parent_path()), ec);
#ifdef _WIN32
    m_file = _wfopen(osPath(file).c_str(), L"wb");
#else
    m_file = std::fopen(file.c_str(), "wb");
#endif
}
FileSink::~FileSink() { close(); }
bool FileSink::write(const uint8_t* data, size_t size) { return m_file && std::fwrite(data, 1, size, m_file) == size; }
void FileSink::close() {
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
}

// ---------------------------------------------------------------- inflate ----
namespace {

class BitReader {
public:
    BitReader(ByteSource& src, uint64_t maxInput) : m_src(src), m_left(maxInput) {}
    // -1 at the end of the input
    int byte() {
        if (m_pos == m_len) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(sizeof(m_buf), m_left));
            m_len = want ? m_src.read(m_buf, want) : 0;
            m_left -= m_len;
            m_pos = 0;
            if (m_len == 0) return -1;
        }
        return m_buf[m_pos++];
    }
    bool need(int n) {
        while (m_bitcnt < n) {
            int b = byte();
            if (b < 0) return false;
            m_bitbuf |= static_cast<uint64_t>(b) << m_bitcnt;
            m_bitcnt += 8;
        }
        return true;
    }
    // n <= 32
    bool bits(int n, uint32_t& v) {
        if (!need(n)) return false;
        v = static_cast<uint32_t>(m_bitbuf & ((1ull << n) - 1));
        m_bitbuf >>= n;
        m_bitcnt -= n;
        return true;
    }
    int available() const { return m_bitcnt; }
    uint64_t peek() const { return m_bitbuf; }
    void drop(int n) {
        m_bitbuf >>= n;
        m_bitcnt -= n;
    }
    void alignByte() { drop(m_bitcnt & 7); }
    // byte-aligned read (after alignByte): buffered bits first
    int alignedByte() {
        if (m_bitcnt >= 8) {
            int b = static_cast<int>(m_bitbuf & 0xFF);
            drop(8);
            return b;
        }
        return byte();
    }

private:
    ByteSource& m_src;
    uint64_t m_left;
    uint8_t m_buf[1 << 16];
    size_t m_pos = 0, m_len = 0;
    uint64_t m_bitbuf = 0;
    int m_bitcnt = 0;
};

struct Huffman {
    static constexpr int kFastBits = 10;
    std::array<uint16_t, 16> count{};
    std::array<uint16_t, 320> symbol{};
    std::array<uint16_t, 1 << kFastBits> fast{};  // (symbol << 4) | length, 0 = slow path
    bool build(const uint16_t* lengths, int n) {
        count.fill(0);
        fast.fill(0);
        for (int i = 0; i < n; ++i) count[lengths[i]]++;
        if (count[0] == n) return true;  // no codes (allowed, e.g. a distance-free block)
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;  // over-subscribed
        }
        std::array<uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        // fast table for short codes (canonical codes are MSB-first in the stream: reverse them)
        uint32_t code = 0;
        std::array<uint32_t, 16> next{};
        for (int len = 1; len < 16; ++len) {
            code = (code + count[len - 1]) << 1;
            next[len] = code;
        }
        for (int i = 0; i < n; ++i) {
            int len = lengths[i];
            if (!len || len > kFastBits) continue;
            uint32_t c = next[len]++, r = 0;
            for (int b = 0; b < len; ++b) r |= ((c >> b) & 1u) << (len - 1 - b);
            for (uint32_t k = r; k < (1u << kFastBits); k += 1u << len) fast[k] = static_cast<uint16_t>((i << 4) | len);
        }
        return true;
    }
    int decode(BitReader& br) const {
        br.need(kFastBits);  // may get fewer near the end of the input
        if (br.available() >= kFastBits || br.available() > 0) {
            uint16_t f = fast[br.peek() & ((1u << kFastBits) - 1)];
            if (f && (f & 15) <= br.available()) {
                br.drop(f & 15);
                return f >> 4;
            }
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            uint32_t bit;
            if (!br.bits(1, bit)) return -1;
            code |= static_cast<int>(bit);
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class Inflater {
public:
    Inflater(BitReader& br, const Sink& out, uint64_t maxOutput) : m_br(br), m_out(out), m_max(maxOutput) {}
    uint64_t produced() const { return m_pos; }
    uint32_t crc() const { return m_crc; }
    Status run() {
        uint32_t last = 0, type = 0;
        do {
            if (!m_br.bits(1, last) || !m_br.bits(2, type)) return fail("truncated deflate stream");
            Status st;
            if (type == 0) st = stored();
            else if (type == 1) st = fixed();
            else if (type == 2) st = dynamic();
            else return fail("invalid deflate block type");
            if (!st) return st;
        } while (!last);
        return flush() ? Status::success() : fail(m_sinkError.empty() ? "output rejected" : m_sinkError);
    }

private:
    static constexpr size_t kWindow = 32768;
    BitReader& m_br;
    const Sink& m_out;
    uint64_t m_max;
    std::array<uint8_t, kWindow> m_ring{};
    uint64_t m_pos = 0, m_flushed = 0;
    uint32_t m_crc = 0;
    std::string m_sinkError;

    static Status fail(const std::string& e) { return Status::failure("deflate: " + e); }
    bool flush() {
        while (m_flushed < m_pos) {
            size_t at = static_cast<size_t>(m_flushed % kWindow);
            size_t n = static_cast<size_t>(std::min<uint64_t>(m_pos - m_flushed, kWindow - at));
            m_crc = crc32(std::span(m_ring.data() + at, n), m_crc);
            if (!m_out(m_ring.data() + at, n)) return false;
            m_flushed += n;
        }
        return true;
    }
    bool put(uint8_t b) {
        if (m_pos >= m_max) return false;
        m_ring[static_cast<size_t>(m_pos % kWindow)] = b;
        ++m_pos;
        return m_pos - m_flushed < kWindow || flush();
    }
    Status stored() {
        m_br.alignByte();
        int b[4];
        for (int& x : b)
            if ((x = m_br.alignedByte()) < 0) return fail("truncated stored block");
        unsigned len = static_cast<unsigned>(b[0] | (b[1] << 8)), nlen = static_cast<unsigned>(b[2] | (b[3] << 8));
        if (len != (~nlen & 0xFFFFu)) return fail("stored block length check failed");
        while (len--) {
            int x = m_br.alignedByte();
            if (x < 0) return fail("truncated stored block");
            if (!put(static_cast<uint8_t>(x))) return fail("output limit exceeded");
        }
        return {};
    }
    Status codes(const Huffman& lit, const Huffman& dist) {
        for (;;) {
            int sym = lit.decode(m_br);
            if (sym < 0) return fail("invalid literal/length code");
            if (sym < 256) {
                if (!put(static_cast<uint8_t>(sym))) return fail(m_pos >= m_max ? "output limit exceeded" : "output rejected");
                continue;
            }
            if (sym == 256) return {};
            sym -= 257;
            if (sym >= 29) return fail("invalid length symbol");
            uint32_t extra = 0;
            if (!m_br.bits(kLenExtra[sym], extra)) return fail("truncated length");
            unsigned len = kLenBase[sym] + extra;
            int ds = dist.decode(m_br);
            if (ds < 0 || ds >= 30) return fail("invalid distance code");
            if (!m_br.bits(kDistExtra[ds], extra)) return fail("truncated distance");
            uint64_t d = kDistBase[ds] + extra;
            if (d > m_pos || d > kWindow) return fail("distance too far back");
            while (len--) {
                uint8_t b = m_ring[static_cast<size_t>((m_pos - d) % kWindow)];
                if (!put(b)) return fail(m_pos >= m_max ? "output limit exceeded" : "output rejected");
            }
        }
    }
    Status fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t;
            uint16_t l[288];
            for (int i = 0; i < 144; ++i) l[i] = 8;
            for (int i = 144; i < 256; ++i) l[i] = 9;
            for (int i = 256; i < 280; ++i) l[i] = 7;
            for (int i = 280; i < 288; ++i) l[i] = 8;
            t.first.build(l, 288);
            uint16_t d[30];
            for (auto& x : d) x = 5;
            t.second.build(d, 30);
            return t;
        }();
        return codes(tables.first, tables.second);
    }
    Status dynamic() {
        uint32_t nlen, ndist, ncode;
        if (!m_br.bits(5, nlen) || !m_br.bits(5, ndist) || !m_br.bits(4, ncode)) return fail("truncated dynamic header");
        nlen += 257;
        ndist += 1;
        ncode += 4;
        if (nlen > 286 || ndist > 30) return fail("bad code counts");
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint16_t lengths[320] = {};
        for (uint32_t i = 0; i < ncode; ++i) {
            uint32_t v;
            if (!m_br.bits(3, v)) return fail("truncated code lengths");
            lengths[kOrder[i]] = static_cast<uint16_t>(v);
        }
        Huffman lencode;
        if (!lencode.build(lengths, 19)) return fail("bad code-length code");
        uint32_t index = 0;
        std::memset(lengths, 0, sizeof(lengths));
        while (index < nlen + ndist) {
            int sym = lencode.decode(m_br);
            if (sym < 0) return fail("invalid code-length symbol");
            if (sym < 16) {
                lengths[index++] = static_cast<uint16_t>(sym);
                continue;
            }
            uint16_t len = 0;
            uint32_t rep = 0;
            if (sym == 16) {
                if (index == 0) return fail("repeat with no previous length");
                len = lengths[index - 1];
                if (!m_br.bits(2, rep)) return fail("truncated repeat");
                rep += 3;
            } else if (sym == 17) {
                if (!m_br.bits(3, rep)) return fail("truncated repeat");
                rep += 3;
            } else {
                if (!m_br.bits(7, rep)) return fail("truncated repeat");
                rep += 11;
            }
            if (index + rep > nlen + ndist) return fail("too many code lengths");
            while (rep--) lengths[index++] = len;
        }
        if (lengths[256] == 0) return fail("no end-of-block code");
        Huffman lit, dist;
        if (!lit.build(lengths, static_cast<int>(nlen))) return fail("bad literal/length code");
        if (!dist.build(lengths + nlen, static_cast<int>(ndist))) return fail("bad distance code");
        return codes(lit, dist);
    }
};

} // namespace

Status inflateRaw(ByteSource& in, const Sink& out, uint64_t maxOutput, uint64_t maxInput, uint32_t* crcOut) {
    auto br = std::make_unique<BitReader>(in, maxInput);
    Inflater inf(*br, out, maxOutput);
    Status st = inf.run();
    if (st && crcOut) *crcOut = inf.crc();
    return st;
}

Status gunzip(ByteSource& in, const Sink& out, const Limits& limits) {
    auto br = std::make_unique<BitReader>(in, limits.maxArchiveBytes);
    uint64_t total = 0;
    std::string limitError;
    Sink guarded = [&](const uint8_t* d, size_t n) {
        total += n;
        if (total > limits.maxTotalBytes) {
            limitError = std::format("archive expands past the {} MiB limit", limits.maxTotalBytes >> 20);
            return false;
        }
        uint64_t base = std::max<uint64_t>(in.consumed, 4ull << 20);
        if (static_cast<double>(total) > limits.maxRatio * static_cast<double>(base)) {
            limitError = std::format("compression ratio above {}:1 (decompression bomb?)", limits.maxRatio);
            return false;
        }
        return out(d, n);
    };
    int members = 0;
    for (;;) {
        int id1 = br->alignedByte();
        if (id1 < 0) {
            if (members == 0) return Status::failure("gzip: empty input");
            return {};
        }
        int id2 = br->alignedByte(), cm = br->alignedByte(), flg = br->alignedByte();
        if (id1 != 0x1f || id2 != 0x8b) {
            if (members > 0 && id1 == 0) return {};  // zero padding after the last member
            return Status::failure("not a gzip file");
        }
        if (cm != 8) return Status::failure("gzip: unsupported compression method");
        if (flg < 0 || (flg & 0xE0)) return Status::failure("gzip: reserved flags set");
        for (int i = 0; i < 6; ++i)
            if (br->alignedByte() < 0) return Status::failure("gzip: truncated header");
        if (flg & 4) {  // FEXTRA
            int lo = br->alignedByte(), hi = br->alignedByte();
            if (lo < 0 || hi < 0) return Status::failure("gzip: truncated header");
            for (int n = lo | (hi << 8); n > 0; --n)
                if (br->alignedByte() < 0) return Status::failure("gzip: truncated header");
        }
        for (int f : {8, 16})  // FNAME, FCOMMENT: zero-terminated
            if (flg & f)
                for (size_t n = 0;; ++n) {
                    int c = br->alignedByte();
                    if (c < 0 || n > 65536) return Status::failure("gzip: bad header string");
                    if (c == 0) break;
                }
        if (flg & 2)
            for (int i = 0; i < 2; ++i)
                if (br->alignedByte() < 0) return Status::failure("gzip: truncated header");
        Inflater inf(*br, guarded, limits.maxTotalBytes + 1);
        uint64_t before = total;
        Status st = inf.run();
        if (!st) return Status::failure(limitError.empty() ? "gzip: " + st.error : limitError);
        br->alignByte();
        uint8_t tr[8];
        for (auto& b : tr) {
            int x = br->alignedByte();
            if (x < 0) return Status::failure("gzip: truncated trailer");
            b = static_cast<uint8_t>(x);
        }
        uint32_t crc = static_cast<uint32_t>(tr[0] | (tr[1] << 8) | (tr[2] << 16)) | (static_cast<uint32_t>(tr[3]) << 24);
        uint32_t isize = static_cast<uint32_t>(tr[4] | (tr[5] << 8) | (tr[6] << 16)) | (static_cast<uint32_t>(tr[7]) << 24);
        if (crc != inf.crc()) return Status::failure("gzip: CRC mismatch (corrupt download?)");
        if (isize != static_cast<uint32_t>(total - before)) return Status::failure("gzip: size mismatch (corrupt download?)");
        ++members;
    }
}

Result<Bytes> gunzipBuffer(std::span<const uint8_t> data, const Limits& limits) {
    MemorySource src(data);
    Bytes out;
    Status st = gunzip(src, [&](const uint8_t* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    }, limits);
    if (!st) return Result<Bytes>::failure(st.error);
    return out;
}

Result<Bytes> inflateBuffer(std::span<const uint8_t> raw, uint64_t maxOutput) {
    MemorySource src(raw);
    Bytes out;
    Status st = inflateRaw(src, [&](const uint8_t* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    }, maxOutput);
    if (!st) return Result<Bytes>::failure(st.error);
    return out;
}

// -------------------------------------------------------------------- tar ----
namespace {
std::optional<uint64_t> tarNumber(const uint8_t* f, size_t n) {
    if (f[0] & 0x80) {  // GNU base-256
        uint64_t v = f[0] & 0x7F;
        for (size_t i = 1; i < n; ++i) {
            if (v >> 55) return std::nullopt;
            v = (v << 8) | f[i];
        }
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (f[i] == ' ' || f[i] == 0)) ++i;
    bool any = false;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; ++i) {
        if (v >> 60) return std::nullopt;
        v = v * 8 + static_cast<uint64_t>(f[i] - '0');
        any = true;
    }
    for (; i < n; ++i)
        if (f[i] != ' ' && f[i] != 0) return std::nullopt;
    return any ? std::optional<uint64_t>(v) : std::optional<uint64_t>(0);
}
std::string tarString(const uint8_t* f, size_t n) {
    size_t len = 0;
    while (len < n && f[len]) ++len;
    return std::string(reinterpret_cast<const char*>(f), len);
}
} // namespace

TarParser::TarParser(Limits limits, OnEntry onEntry, std::function<void(const TarEntry&)> onEnd)
    : m_limits(limits), m_onEntry(std::move(onEntry)), m_onEnd(std::move(onEnd)) {}

bool TarParser::feed(const uint8_t* data, size_t size) {
    if (!m_error.empty()) return false;
    while (size) {
        switch (m_state) {
            case State::Done: return true;  // trailing zero padding
            case State::Header: {
                size_t n = std::min(size, sizeof(m_block) - m_blockFill);
                std::memcpy(m_block + m_blockFill, data, n);
                m_blockFill += n;
                data += n;
                size -= n;
                if (m_blockFill == sizeof(m_block)) {
                    m_blockFill = 0;
                    if (!header()) return false;
                }
                break;
            }
            case State::Data:
            case State::LongName:
            case State::Pax: {
                if (m_remaining) {
                    size_t n = static_cast<size_t>(std::min<uint64_t>(size, m_remaining));
                    if (m_state == State::Data) {
                        if (m_sink && !m_sink(data, n)) {
                            m_error = "tar: cannot write " + m_cur.path;
                            return false;
                        }
                    } else {
                        m_meta.append(reinterpret_cast<const char*>(data), n);
                    }
                    m_remaining -= n;
                    data += n;
                    size -= n;
                    break;
                }
                size_t n = static_cast<size_t>(std::min<uint64_t>(size, m_padding));
                m_padding -= n;
                data += n;
                size -= n;
                break;
            }
        }
        if ((m_state == State::Data || m_state == State::LongName || m_state == State::Pax) && m_remaining == 0 && m_padding == 0 &&
            !complete())
            return false;
    }
    return true;
}

bool TarParser::complete() {
    if (m_state == State::Data) {
        if (m_sink && m_onEnd) m_onEnd(m_cur);
    } else if (m_state == State::LongName) {
        m_nextPath = m_meta.substr(0, m_meta.find('\0'));
    } else if (m_state == State::Pax && m_cur.type == 'x') {
        // records: "<len> key=value\n"
        size_t p = 0;
        while (p < m_meta.size()) {
            size_t sp = m_meta.find(' ', p);
            if (sp == std::string::npos) break;
            size_t len = std::strtoull(m_meta.substr(p, sp - p).c_str(), nullptr, 10);
            if (len <= sp - p + 1 || p + len > m_meta.size()) {
                m_error = "tar: malformed pax header";
                return false;
            }
            std::string rec = m_meta.substr(sp + 1, len - (sp - p) - 2);
            if (auto eq = rec.find('='); eq != std::string::npos) {
                std::string k = rec.substr(0, eq), v = rec.substr(eq + 1);
                if (k == "path") m_nextPath = v;
                else if (k == "size") m_nextSize = std::strtoull(v.c_str(), nullptr, 10);
            }
            p += len;
        }
    }
    m_sink = {};
    m_meta.clear();
    m_state = State::Header;
    return true;
}

bool TarParser::header() {
    const uint8_t* b = m_block;
    if (std::all_of(b, b + 512, [](uint8_t c) { return c == 0; })) {
        if (++m_zeroBlocks >= 2) m_state = State::Done;
        return true;
    }
    m_zeroBlocks = 0;
    auto chk = tarNumber(b + 148, 8);
    uint64_t sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : b[i];
    if (!chk || *chk != sum) {
        m_error = "tar: corrupt header (checksum mismatch)";
        return false;
    }
    auto size = tarNumber(b + 124, 12);
    if (!size) {
        m_error = "tar: bad size field";
        return false;
    }
    char type = static_cast<char>(b[156]);
    uint64_t sz = *size;
    if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
        if (sz > 1 << 20) {
            m_error = "tar: oversized extended header";
            return false;
        }
        m_cur = {};
        m_cur.type = type;
        m_state = (type == 'L') ? State::LongName : State::Pax;
        if (type == 'K') m_state = State::Pax;  // long link name: collected and ignored
        if (type == 'g') m_warnings.push_back("tar: global pax header ignored");
        m_meta.clear();
        m_remaining = sz;
        m_padding = (512 - sz % 512) % 512;
        return true;
    }
    if (++m_entries > m_limits.maxEntries) {
        m_error = std::format("tar: more than {} entries", m_limits.maxEntries);
        return false;
    }
    TarEntry e;
    e.type = type == 0 ? '0' : type;
    std::string name = tarString(b, 100);
    if (std::memcmp(b + 257, "ustar", 5) == 0) {
        std::string prefix = tarString(b + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (!m_nextPath.empty()) name = m_nextPath;
    if (m_nextSize) sz = *m_nextSize;
    m_nextPath.clear();
    m_nextSize.reset();
    e.rawPath = name;
    e.size = sz;
    bool hasData = e.type == '0' || e.type == '7';
    if (e.type == '1' || e.type == '2') e.skipReason = "link (links are never extracted)";
    else if (e.type == '3' || e.type == '4' || e.type == '6') e.skipReason = "device or pipe entry";
    else if (!hasData && e.type != '5') e.skipReason = std::format("unsupported tar entry type '{}'", e.type);
    if (e.skipReason.empty()) {
        if (auto p = sanitizeEntryPath(name, m_limits)) e.path = *p;
        else if (name != "./" && name != ".") e.skipReason = "unsafe path (absolute, '..', device name or invalid characters)";
    }
    if (hasData && sz > m_limits.maxEntryBytes) {
        m_error = std::format("tar: '{}' is larger than the {} MiB per-file limit", name, m_limits.maxEntryBytes >> 20);
        return false;
    }
    if (hasData && (m_total += sz) > m_limits.maxTotalBytes) {
        m_error = std::format("tar: contents exceed the {} MiB limit", m_limits.maxTotalBytes >> 20);
        return false;
    }
    if (!e.skipReason.empty() && !e.rawPath.empty()) m_warnings.push_back(std::format("skipped '{}': {}", e.rawPath, e.skipReason));
    m_cur = e;
    m_sink = {};
    if (m_onEntry && e.skipReason.empty() && (hasData || e.type == '5')) m_sink = m_onEntry(m_cur);
    if (!hasData) m_sink = {};
    uint64_t dataBytes = (e.type == '5') ? 0 : sz;  // directories carry no data; links/devices may (skipped)
    m_state = State::Data;
    m_remaining = dataBytes;
    m_padding = (512 - dataBytes % 512) % 512;
    return true;
}

Status TarParser::finish() const {
    if (!m_error.empty()) return Status::failure(m_error);
    if (m_state == State::Data && (m_remaining || m_padding)) return Status::failure("tar: truncated archive");
    if (m_blockFill != 0) return Status::failure("tar: truncated header");
    return {};
}

// -------------------------------------------------------------------- zip ----
namespace {
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
bool readAt(FileSource& f, uint64_t off, uint8_t* dst, size_t n) { return f.seek(off) && f.read(dst, n) == n; }
} // namespace

Status ZipReader::open(const fs::path& file, const Limits& limits) {
    m_limits = limits;
    m_entries.clear();
    m_written = 0;
    m_src = std::make_unique<FileSource>(file);
    if (!m_src->ok()) return Status::failure("cannot open " + narrow(file.wstring()));
    uint64_t n = m_src->size();
    if (n > limits.maxArchiveBytes) return Status::failure("zip: archive larger than the size limit");
    if (n < 22) return Status::failure("not a zip archive");
    size_t tail = static_cast<size_t>(std::min<uint64_t>(n, 22 + 65535 + 20));
    Bytes t(tail);
    if (!readAt(*m_src, n - tail, t.data(), tail)) return Status::failure("zip: read error");
    size_t eocd = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (rd32(&t[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return Status::failure("not a zip archive (no end of central directory)");
    uint64_t count = rd16(&t[eocd + 10]), cdSize = rd32(&t[eocd + 12]), cdOff = rd32(&t[eocd + 16]);
    if ((count == 0xFFFF || cdOff == 0xFFFFFFFF || cdSize == 0xFFFFFFFF) && eocd >= 20 && rd32(&t[eocd - 20]) == 0x07064b50) {
        uint64_t z64 = rd64(&t[eocd - 20 + 8]);
        uint8_t z[56];
        if (z64 + 56 > n || !readAt(*m_src, z64, z, 56) || rd32(z) != 0x06064b50) return Status::failure("zip: bad ZIP64 record");
        count = rd64(z + 32);
        cdSize = rd64(z + 40);
        cdOff = rd64(z + 48);
    }
    if (count > limits.maxEntries) return Status::failure(std::format("zip: more than {} entries", limits.maxEntries));
    if (cdOff + cdSize > n || cdSize > (256ull << 20)) return Status::failure("zip: central directory out of range");
    Bytes cd(static_cast<size_t>(cdSize));
    if (cdSize && !readAt(*m_src, cdOff, cd.data(), cd.size())) return Status::failure("zip: read error");
    size_t p = 0;
    uint64_t declared = 0;
    std::set<std::string> seen;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) return Status::failure("zip: bad central directory entry");
        ZipEntry e;
        uint16_t madeBy = rd16(&cd[p + 4]);
        e.flags = rd16(&cd[p + 8]);
        e.method = rd16(&cd[p + 10]);
        e.crc = rd32(&cd[p + 16]);
        e.compressedSize = rd32(&cd[p + 20]);
        e.size = rd32(&cd[p + 24]);
        uint16_t nameLen = rd16(&cd[p + 28]), extraLen = rd16(&cd[p + 30]), commentLen = rd16(&cd[p + 32]);
        uint32_t extAttr = rd32(&cd[p + 38]);
        e.localHeader = rd32(&cd[p + 42]);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) return Status::failure("zip: truncated central directory");
        e.rawPath.assign(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        const uint8_t* ex = &cd[p + 46 + nameLen];
        for (size_t k = 0; k + 4 <= extraLen;) {
            uint16_t id = rd16(ex + k), sz = rd16(ex + k + 2);
            if (k + 4 + sz > extraLen) break;
            if (id == 0x0001) {
                size_t q = k + 4, end = k + 4 + sz;
                if (e.size == 0xFFFFFFFF && q + 8 <= end) e.size = rd64(ex + q), q += 8;
                if (e.compressedSize == 0xFFFFFFFF && q + 8 <= end) e.compressedSize = rd64(ex + q), q += 8;
                if (e.localHeader == 0xFFFFFFFF && q + 8 <= end) e.localHeader = rd64(ex + q), q += 8;
            }
            k += 4 + sz;
        }
        p += 46 + nameLen + extraLen + commentLen;
        e.directory = !e.rawPath.empty() && (e.rawPath.back() == '/' || e.rawPath.back() == '\\');
        bool unixHost = (madeBy >> 8) == 3;
        uint32_t mode = extAttr >> 16;
        if (unixHost && (mode & 0170000) == 0120000) e.skipReason = "symbolic link (links are never extracted)";
        else if (e.flags & 1) e.skipReason = "encrypted entry (Fledge does not decrypt archives)";
        else if (!e.directory && e.method != 0 && e.method != 8) e.skipReason = std::format("unsupported compression method {}", e.method);
        else if (e.size > limits.maxEntryBytes) e.skipReason = std::format("larger than the {} MiB per-file limit", limits.maxEntryBytes >> 20);
        else if (e.size > (4ull << 20) && static_cast<double>(e.size) > limits.maxRatio * static_cast<double>(std::max<uint64_t>(e.compressedSize, 1)))
            e.skipReason = "compression ratio too high (decompression bomb?)";
        if (e.skipReason.empty()) {
            if (auto sp = sanitizeEntryPath(e.rawPath, limits)) {
                e.path = *sp;
                if (!e.directory && !seen.insert(toLower(e.path)).second) {
                    e.path.clear();
                    e.skipReason = "duplicate path (case-insensitive)";
                }
            } else {
                e.skipReason = "unsafe path (absolute, '..', device name or invalid characters)";
            }
        }
        if (e.skipReason.empty() && !e.directory) declared += e.size;
        if (e.localHeader + 30 > n || e.localHeader + 30 + e.compressedSize > n) return Status::failure("zip: entry data out of range: " + e.rawPath);
        m_entries.push_back(std::move(e));
    }
    if (declared > limits.maxTotalBytes) return Status::failure(std::format("zip: contents exceed the {} MiB limit", limits.maxTotalBytes >> 20));
    // Overlapping entries (several names pointing at the same compressed data) are a zip-bomb trick.
    std::vector<const ZipEntry*> byOffset;
    for (auto& e : m_entries)
        if (!e.directory && e.skipReason.empty()) byOffset.push_back(&e);
    std::sort(byOffset.begin(), byOffset.end(), [](auto* a, auto* b) { return a->localHeader < b->localHeader; });
    for (size_t i = 1; i < byOffset.size(); ++i) {
        const ZipEntry* a = byOffset[i - 1];
        if (a->localHeader + 30 + a->rawPath.size() + a->compressedSize > byOffset[i]->localHeader)
            return Status::failure("zip: overlapping entries (malformed or a zip bomb)");
    }
    return {};
}

Status ZipReader::extract(const ZipEntry& e, const Sink& out) {
    if (!e.skipReason.empty()) return Status::failure(e.skipReason);
    uint8_t lh[30];
    if (!readAt(*m_src, e.localHeader, lh, 30) || rd32(lh) != 0x04034b50) return Status::failure("zip: bad local header for " + e.rawPath);
    uint64_t data = e.localHeader + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data + e.compressedSize > m_src->size() || !m_src->seek(data)) return Status::failure("zip: data out of range for " + e.rawPath);
    if (m_written + e.size > m_limits.maxTotalBytes) return Status::failure("zip: total size limit reached");
    uint64_t got = 0;
    uint32_t crc = 0;
    Sink counted = [&](const uint8_t* d, size_t n) {
        got += n;
        return got <= e.size && out(d, n);
    };
    if (e.method == 0) {
        if (e.compressedSize != e.size) return Status::failure("zip: stored entry with mismatching sizes: " + e.rawPath);
        uint8_t buf[1 << 16];
        for (uint64_t left = e.size; left;) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(left, sizeof(buf)));
            if (m_src->read(buf, n) != n) return Status::failure("zip: truncated data for " + e.rawPath);
            crc = crc32(std::span(buf, n), crc);
            if (!counted(buf, n)) return Status::failure("zip: cannot write " + e.rawPath);
            left -= n;
        }
    } else {
        Status st = inflateRaw(*m_src, counted, e.size, e.compressedSize, &crc);
        if (!st) return Status::failure(e.rawPath + ": " + st.error);
    }
    if (got != e.size) return Status::failure("zip: size mismatch for " + e.rawPath);
    if (crc != e.crc) return Status::failure("zip: CRC mismatch for " + e.rawPath + " (corrupt download?)");
    m_written += got;
    return {};
}

Result<std::vector<std::string>> ZipReader::extractAll(const fs::path& dest, std::vector<std::string>& warnings) {
    std::vector<std::string> written;
    for (const ZipEntry& e : m_entries) {
        if (!e.skipReason.empty()) {
            warnings.push_back(std::format("skipped '{}': {}", e.rawPath, e.skipReason));
            continue;
        }
        if (e.directory) continue;
        FileSink sink(dest / pathFromUtf8(e.path));
        if (!sink.ok()) return Result<std::vector<std::string>>::failure("cannot create " + e.path);
        Status st = extract(e, [&](const uint8_t* d, size_t n) { return sink.write(d, n); });
        sink.close();
        if (!st) {
            std::error_code ec;
            fs::remove(osPath(dest / pathFromUtf8(e.path)), ec);
            return Result<std::vector<std::string>>::failure(st.error);
        }
        written.push_back(e.path);
    }
    return written;
}

} // namespace fledge::archive
