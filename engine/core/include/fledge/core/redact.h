#pragma once
// Secret redaction. Used by the logger (always), by MCP tool results and by
// anything that echoes user/provider text. Two mechanisms:
//  1. exact secrets registered at runtime (e.g. an API key after it was loaded)
//  2. heuristic patterns for common key/token formats (sk-..., sk-ant-...,
//     "Bearer xxx", "x-api-key: xxx", AIza..., ghp_...).
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fledge {

class Redactor {
public:
    static Redactor& get();

    void registerSecret(std::string_view secret);   // ignored if < 8 chars
    void unregisterSecret(std::string_view secret);
    std::string redact(std::string_view text) const;
    static constexpr const char* kMask = "[REDACTED]";

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_secrets;
};

inline std::string redact(std::string_view text) { return Redactor::get().redact(text); }

} // namespace fledge
