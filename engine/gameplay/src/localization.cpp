#include "fledge/gameplay/localization.h"

#include <cstdlib>

namespace fledge::gameplay {

namespace {
void flatten(const Json& j, const std::string& prefix, std::map<std::string, std::string>& out) {
    if (!j.is_object()) return;
    for (auto& [k, v] : j.items()) {
        std::string key = prefix.empty() ? k : prefix + "." + k;
        if (v.is_object()) flatten(v, key, out);
        else if (v.is_string()) out[key] = v.get<std::string>();
        else if (v.is_number() || v.is_boolean()) out[key] = v.dump();
    }
}
} // namespace

void Localization::addTable(const std::string& language, const Json& table) { flatten(table, "", m_tables[language]); }
void Localization::clear() { m_tables.clear(); }

std::vector<std::string> Localization::languages() const {
    std::vector<std::string> out;
    for (auto& [l, t] : m_tables) out.push_back(l);
    return out;
}

bool Localization::has(const std::string& key) const {
    for (const std::string* l : {&m_language, &m_fallback})
        if (auto t = m_tables.find(*l); t != m_tables.end() && t->second.count(key)) return true;
    return false;
}

std::string Localization::tr(const std::string& key, const Args& args) const {
    for (const std::string* l : {&m_language, &m_fallback})
        if (auto t = m_tables.find(*l); t != m_tables.end())
            if (auto it = t->second.find(key); it != t->second.end()) return formatMessage(it->second, args);
    return key;
}

std::string Localization::text(const std::string& s, const Args& args) const {
    if (s.size() > 1 && s[0] == '@') return tr(s.substr(1), args);
    return s;
}

size_t Localization::size(const std::string& language) const {
    auto t = m_tables.find(language);
    return t == m_tables.end() ? 0 : t->second.size();
}

std::vector<std::string> Localization::missing(const std::string& language) const {
    std::vector<std::string> out;
    auto fb = m_tables.find(m_fallback);
    if (fb == m_tables.end()) return out;
    auto t = m_tables.find(language);
    for (auto& [k, v] : fb->second)
        if (t == m_tables.end() || !t->second.count(k)) out.push_back(k);
    return out;
}

std::string formatMessage(const std::string& p, const Localization::Args& args) {
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] != '{') {
            out += p[i];
            continue;
        }
        // find the matching '}' (plural forms may nest {name} placeholders)
        int depth = 0;
        size_t end = i;
        for (; end < p.size(); ++end) {
            if (p[end] == '{') ++depth;
            else if (p[end] == '}' && --depth == 0) break;
        }
        if (end >= p.size()) {  // unbalanced: keep the rest literally
            out += p.substr(i);
            break;
        }
        std::string body = p.substr(i + 1, end - i - 1);
        size_t bar = body.find('|');
        std::string name = body.substr(0, bar);
        auto a = args.find(name);
        if (bar != std::string::npos) {  // {count|one|other}
            std::string forms = body.substr(bar + 1);
            size_t bar2 = forms.find('|');
            std::string one = forms.substr(0, bar2), other = bar2 == std::string::npos ? forms : forms.substr(bar2 + 1);
            long n = a != args.end() ? std::strtol(a->second.c_str(), nullptr, 10) : 0;
            out += formatMessage(n == 1 ? one : other, args);
        } else if (a != args.end()) {
            out += a->second;
        } else {
            out += "{" + body + "}";  // unknown placeholders stay visible
        }
        i = end;
    }
    return out;
}

} // namespace fledge::gameplay
