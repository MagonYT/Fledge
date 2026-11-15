// Minimal JSON value used inside script modules to exchange property values
// with the engine. Header-only, no dependencies (keeps script builds fast).
#pragma once
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fledge::script::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_type(Type::Bool), m_bool(b) {}
    Value(int n) : m_type(Type::Number), m_num(n) {}
    Value(unsigned n) : m_type(Type::Number), m_num(n) {}
    Value(float n) : m_type(Type::Number), m_num(n) {}
    Value(double n) : m_type(Type::Number), m_num(n) {}
    Value(const char* s) : m_type(Type::String), m_str(s) {}
    Value(std::string s) : m_type(Type::String), m_str(std::move(s)) {}
    static Value array() { Value v; v.m_type = Type::Array; return v; }
    static Value object() { Value v; v.m_type = Type::Object; return v; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool def = false) const { return m_type == Type::Bool ? m_bool : def; }
    double asNumber(double def = 0) const { return m_type == Type::Number ? m_num : def; }
    const std::string& asString() const { return m_str; }

    // arrays
    size_t size() const { return m_type == Type::Array ? m_items.size() : m_type == Type::Object ? m_members.size() : 0; }
    const Value& operator[](size_t i) const { return i < m_items.size() ? m_items[i] : null(); }
    void push(Value v) {
        m_type = Type::Array;
        m_items.push_back(std::move(v));
    }
    const std::vector<Value>& items() const { return m_items; }
    // objects
    const Value* find(std::string_view key) const {
        for (auto& [k, v] : m_members)
            if (k == key) return &v;
        return nullptr;
    }
    const Value& operator[](std::string_view key) const {
        const Value* v = find(key);
        return v ? *v : null();
    }
    void set(std::string key, Value v) {
        m_type = Type::Object;
        for (auto& [k, old] : m_members)
            if (k == key) {
                old = std::move(v);
                return;
            }
        m_members.emplace_back(std::move(key), std::move(v));
    }
    const std::vector<std::pair<std::string, Value>>& members() const { return m_members; }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }
    static Value parse(std::string_view text, bool* ok = nullptr) {
        size_t i = 0;
        bool good = true;
        Value v = parseValue(text, i, good, 0);
        skipWs(text, i);
        if (i != text.size()) good = false;
        if (ok) *ok = good;
        return good ? v : Value();
    }

private:
    static const Value& null() {
        static const Value n;
        return n;
    }
    static void writeString(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }
    void write(std::string& out) const {
        switch (m_type) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += m_bool ? "true" : "false"; break;
            case Type::Number: {
                if (!std::isfinite(m_num)) {
                    out += "0";
                    break;
                }
                char buf[32];
                if (m_num == std::floor(m_num) && std::fabs(m_num) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", m_num);
                else std::snprintf(buf, sizeof buf, "%.9g", m_num);
                out += buf;
                break;
            }
            case Type::String: writeString(out, m_str); break;
            case Type::Array:
                out += '[';
                for (size_t i = 0; i < m_items.size(); ++i) {
                    if (i) out += ',';
                    m_items[i].write(out);
                }
                out += ']';
                break;
            case Type::Object:
                out += '{';
                for (size_t i = 0; i < m_members.size(); ++i) {
                    if (i) out += ',';
                    writeString(out, m_members[i].first);
                    out += ':';
                    m_members[i].second.write(out);
                }
                out += '}';
                break;
        }
    }
    static void skipWs(std::string_view t, size_t& i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    static std::string parseString(std::string_view t, size_t& i, bool& ok) {
        std::string s;
        ++i;  // opening quote
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\' && i < t.size()) {
                char e = t[i++];
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        if (i + 4 > t.size()) { ok = false; return s; }
                        unsigned cp = static_cast<unsigned>(std::strtoul(std::string(t.substr(i, 4)).c_str(), nullptr, 16));
                        i += 4;
                        if (cp < 0x80) s += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            s += static_cast<char>(0xC0 | (cp >> 6));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (cp >> 12));
                            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: s += e;
                }
            } else {
                s += c;
            }
        }
        if (i >= t.size()) ok = false;
        else ++i;  // closing quote
        return s;
    }
    static Value parseValue(std::string_view t, size_t& i, bool& ok, int depth) {
        skipWs(t, i);
        if (i >= t.size() || depth > 64) {
            ok = false;
            return {};
        }
        char c = t[i];
        if (c == '{') {
            Value v = object();
            ++i;
            skipWs(t, i);
            if (i < t.size() && t[i] == '}') {
                ++i;
                return v;
            }
            while (ok) {
                skipWs(t, i);
                if (i >= t.size() || t[i] != '"') { ok = false; break; }
                std::string key = parseString(t, i, ok);
                skipWs(t, i);
                if (i >= t.size() || t[i] != ':') { ok = false; break; }
                ++i;
                v.set(std::move(key), parseValue(t, i, ok, depth + 1));
                skipWs(t, i);
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; break; }
                ok = false;
            }
            return v;
        }
        if (c == '[') {
            Value v = array();
            ++i;
            skipWs(t, i);
            if (i < t.size() && t[i] == ']') {
                ++i;
                return v;
            }
            while (ok) {
                v.push(parseValue(t, i, ok, depth + 1));
                skipWs(t, i);
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; break; }
                ok = false;
            }
            return v;
        }
        if (c == '"') return Value(parseString(t, i, ok));
        if (t.substr(i, 4) == "true") { i += 4; return Value(true); }
        if (t.substr(i, 5) == "false") { i += 5; return Value(false); }
        if (t.substr(i, 4) == "null") { i += 4; return Value(); }
        size_t start = i;
        while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '-' || t[i] == '+' || t[i] == '.' || t[i] == 'e' || t[i] == 'E'))
            ++i;
        if (start == i) {
            ok = false;
            return {};
        }
        return Value(std::strtod(std::string(t.substr(start, i - start)).c_str(), nullptr));
    }

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_num = 0;
    std::string m_str;
    std::vector<Value> m_items;
    std::vector<std::pair<std::string, Value>> m_members;
};

} // namespace fledge::script::json
