// Identifier / string-literal / comment sanitising: everything that reaches the generated C++ from the input goes through
// one of these (no code injection through pin names, node names or default values).
#include <cctype>
#include <set>

#include "fledge/blueprint/blueprint.h"

namespace fledge::blueprint {

namespace {
const std::set<std::string>& reserved() {
    static const std::set<std::string> r = {
        // C++ keywords and alternative tokens
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "final", "override", "import", "module",
        // standard macros / names that would break generated code
        "NULL", "EOF", "assert", "errno", "offsetof", "stdin", "stdout", "stderr", "INT_MAX", "INT_MIN", "RAND_MAX", "main", "std",
        "size_t", "min", "max", "abs",
        // Fledge Script SDK names reachable through `using namespace fledge::script`
        "fledge", "script", "perch", "detail", "json", "bp", "Scene", "Time", "Input", "Log", "Entity", "Audio", "AudioSource", "Animator",
        "Vehicle", "Mission", "Gameplay", "RigidBody", "CharacterController", "Range", "Tooltip", "AssetKind", "Hidden", "ReadOnly",
        "Vec3", "Quat", "Color", "Task", "Events", "Object", "Behaviour", "Service", "Collision", "RayHit", "AssetRef", "BoneTransform",
        "Radians", "Degrees", "Clamp", "Lerp", "Damp", "kPi", "WaitSeconds", "WaitFrames", "NextFrame", "WaitUntil", "WaitForEvent",
        "WaitState", "GetService", "TimerHandle", "SubscriptionId", "CoroutineHandle", "PropertyTraits", "PropertyOptions", "IsVector",
        // Behaviour / Object members
        "GetEntity", "IsEnabled", "SetEnabled", "GetBehaviour", "GetBehaviours", "AddBehaviour", "IsHotReloading", "StartCoroutine",
        "StopCoroutine", "StopAllCoroutines", "After", "Every", "CancelTimer", "Subscribe", "ClassName", "OnCreate", "OnStart", "OnUpdate",
        "OnFixedUpdate", "OnLateUpdate", "OnEnable", "OnDisable", "OnDestroy", "OnHotReload", "OnCollisionEnter", "OnCollisionExit",
        "OnTriggerEnter", "OnTriggerExit", "OnStop",
        // names the generated code uses as locals / parameters
        "dt", "other", "c", "e", "bp_src", "bp_xf"};
    return r;
}
} // namespace

bool isReservedIdentifier(std::string_view id) {
    std::string s(id);
    if (reserved().contains(s)) return true;
    if (s.find("__") != std::string::npos) return true;
    if (s.size() >= 2 && s[0] == '_' && std::isupper(static_cast<unsigned char>(s[1]))) return true;
    for (const char* p : {"FLEDGE_", "FSCRIPT_", "PERCH_", "fledge_script_", "bp_sel_", "bp_gen_"})
        if (startsWith(s, p)) return true;
    return false;
}

std::string sanitizeIdentifier(std::string_view text, std::string_view fallback) {
    std::string out;
    bool pendingUnderscore = false;
    for (unsigned char c : text) {
        if (std::isalnum(c) && c < 0x80) {
            if (pendingUnderscore && !out.empty()) out += '_';
            pendingUnderscore = false;
            out += static_cast<char>(c);
        } else {
            pendingUnderscore = true;  // runs of anything else (spaces, punctuation, UTF-8) become one '_'
        }
        if (out.size() >= 64) break;
    }
    if (out.empty()) {
        if (fallback.empty() || fallback == "Value") return "Value";
        out = sanitizeIdentifier(fallback, "Value");
    }
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "n" + out;
    if (out.size() > 64) out.resize(64);
    while (isReservedIdentifier(out)) {
        if (out.find("__") != std::string::npos) {
            std::string t;
            for (char ch : out)
                if (!(ch == '_' && !t.empty() && t.back() == '_')) t += ch;
            out = t;
            continue;
        }
        if (out[0] == '_') {
            out.erase(0, 1);
            if (out.empty()) out = "Value";
            continue;
        }
        out = "bp" + out;  // "class" -> "bpclass", "FLEDGE_X" -> "bpFLEDGE_X"
        if (out.size() > 66) out.resize(66);
    }
    return out;
}

std::string cppStringLiteral(std::string_view text) {
    std::string out = "\"";
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '?': out += "\\?"; break;  // no trigraph-like sequences
            default:
                if (c < 0x20 || c >= 0x7f) {
                    char buf[5] = {'\\', static_cast<char>('0' + ((c >> 6) & 7)), static_cast<char>('0' + ((c >> 3) & 7)), static_cast<char>('0' + (c & 7)), 0};
                    out += buf;  // octal escapes are at most 3 digits: never swallow the next character
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

std::string commentText(std::string_view text, size_t maxLen) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (c < 0x20 || c >= 0x7f || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        char ch = static_cast<char>(c);
        if (ch == '\\') ch = '/';  // a trailing backslash would splice the next line into the comment
        if (ch == '/' && !out.empty() && out.back() == '*') out += ' ';  // "*/"
        if (ch == '*' && !out.empty() && out.back() == '/') out += ' ';  // "/*"
        out += ch;
        if (out.size() >= maxLen) {
            out += "...";
            break;
        }
    }
    return out;
}

bool isSafeSubfolder(std::string_view folder) {
    if (folder.empty()) return true;
    if (folder.size() > 120) return false;
    if (folder.front() == '/' || folder.front() == '\\' || folder.find(':') != std::string_view::npos) return false;
    std::string part;
    auto ok = [](const std::string& p) {
        if (p.empty() || p == "." || p == "..") return false;
        if (p.back() == '.' || p.back() == ' ' || p.front() == ' ') return false;
        std::string u = toLower(p);
        for (const char* d : {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2",
                              "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"})
            if (u == d) return false;
        return true;
    };
    for (char c : folder) {
        if (c == '/' || c == '\\') {
            if (!ok(part)) return false;
            part.clear();
            continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ' || c == '.')) return false;
        part += c;
    }
    return ok(part);
}

} // namespace fledge::blueprint
