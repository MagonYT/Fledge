#include "fledge/core/redact.h"

#include <algorithm>
#include <cctype>

namespace fledge {

Redactor& Redactor::get() {
    static Redactor r;
    return r;
}

void Redactor::registerSecret(std::string_view secret) {
    if (secret.size() < 8) return;
    std::lock_guard lock(m_mutex);
    if (std::find(m_secrets.begin(), m_secrets.end(), secret) == m_secrets.end())
        m_secrets.emplace_back(secret);
    // longest first so overlapping secrets are fully masked
    std::sort(m_secrets.begin(), m_secrets.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
}

void Redactor::unregisterSecret(std::string_view secret) {
    std::lock_guard lock(m_mutex);
    std::erase(m_secrets, std::string(secret));
}

namespace {

bool isTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}

bool startsWithAt(std::string_view s, size_t pos, std::string_view prefix, bool caseInsensitive = false) {
    if (pos + prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[pos + i], b = prefix[i];
        if (caseInsensitive) {
            a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
            b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
        }
        if (a != b) return false;
    }
    return true;
}

// Mask token-looking runs that begin with known prefixes.
std::string maskPatterns(std::string_view in) {
    static const std::string_view kPrefixes[] = {"sk-ant-", "sk-proj-", "sk-", "AIza", "ghp_", "gho_", "xoxb-", "hf_"};
    static const std::string_view kLabels[] = {"bearer ", "x-api-key:", "x-api-key: ", "api_key=", "apikey=",
                                               "authorization: bearer "};
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        bool boundary = (i == 0) || !isTokenChar(in[i - 1]);
        bool matched = false;
        if (boundary) {
            for (auto label : kLabels) {
                if (startsWithAt(in, i, label, true)) {
                    size_t j = i + label.size();
                    while (j < in.size() && in[j] == ' ') ++j;
                    size_t k = j;
                    while (k < in.size() && isTokenChar(in[k])) ++k;
                    if (k - j >= 8) {
                        out.append(in.substr(i, j - i));
                        out.append(Redactor::kMask);
                        i = k;
                        matched = true;
                    }
                    break;
                }
            }
            if (!matched) {
                for (auto p : kPrefixes) {
                    if (startsWithAt(in, i, p)) {
                        size_t k = i + p.size();
                        while (k < in.size() && isTokenChar(in[k])) ++k;
                        if (k - i >= p.size() + 16) {
                            out.append(Redactor::kMask);
                            i = k;
                            matched = true;
                        }
                        break;
                    }
                }
            }
        }
        if (!matched) out.push_back(in[i++]);
    }
    return out;
}

} // namespace

std::string Redactor::redact(std::string_view text) const {
    std::string s(text);
    {
        std::lock_guard lock(m_mutex);
        for (const auto& secret : m_secrets) {
            size_t pos = 0;
            while ((pos = s.find(secret, pos)) != std::string::npos) {
                s.replace(pos, secret.size(), kMask);
                pos += std::char_traits<char>::length(kMask);
            }
        }
    }
    return maskPatterns(s);
}

} // namespace fledge
