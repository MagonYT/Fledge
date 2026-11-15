#pragma once
// Localisation (docs/GAMEPLAY.md "UI"): string tables per language, loaded from Content/Localization/<lang>.json
// ({"menu": {"play": "Play"}} -> key "menu.play"). tr() looks the key up in the current language, then the
// fallback language, then returns the key itself (so missing strings are visible, never empty). Arguments fill
// {name} placeholders; "{count|one item|{count} items}" picks a plural form by an integer argument (English-style
// one / other). UI text starting with '@' is a key: "@menu.play".
#include <map>
#include <string>
#include <vector>

#include "fledge/core/json.h"

namespace fledge::gameplay {

class Localization {
public:
    using Args = std::map<std::string, std::string>;
    void addTable(const std::string& language, const Json& table);  // merges; nested objects flatten with '.'
    void clear();
    void setLanguage(const std::string& language) { m_language = language; }
    const std::string& language() const { return m_language; }
    void setFallback(const std::string& language) { m_fallback = language; }
    const std::string& fallback() const { return m_fallback; }
    std::vector<std::string> languages() const;
    bool has(const std::string& key) const;  // in the current or the fallback language
    std::string tr(const std::string& key, const Args& args = {}) const;
    // UI strings: "@key" is translated (arguments apply), anything else is returned as is.
    std::string text(const std::string& s, const Args& args = {}) const;
    size_t size(const std::string& language) const;
    // Keys present in the fallback language but missing in `language` (translation status).
    std::vector<std::string> missing(const std::string& language) const;

private:
    std::map<std::string, std::map<std::string, std::string>> m_tables;  // language -> key -> text
    std::string m_language = "en", m_fallback = "en";
};

// Placeholder / plural substitution on its own (tests, HUD bindings).
std::string formatMessage(const std::string& pattern, const Localization::Args& args);

} // namespace fledge::gameplay
