// Graph IR -> Fledge Script behaviour. Exec flow becomes structured C++ (if / for / switch), pure nodes become inline
// expressions, impure node outputs live in members (Blueprint values persist after the node ran), latent nodes (Delay)
// turn their event into a coroutine, merges in plain events become private helper functions, and anything without a
// Fledge equivalent becomes a typed TODO(blueprint) stub so the file always compiles.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <set>

#include "fledge/blueprint/blueprint.h"

namespace fledge::blueprint {

namespace {
const char* kPrelude =
#include "prelude.inl"
    ;

// ---------------------------------------------------------------------------------------------- types --
enum class K { Void, Bool, Int, Int64, Float, String, Name, Text, Vector, Rotator, Transform, Color, Entity, Class, Asset, Timer, Opaque };
struct Ty {
    K k = K::Void;
    char cont = 0;  // 0, 'A' array, 'S' set, 'M' map
    K val = K::Void;
    bool operator==(const Ty&) const = default;
};
bool isAssetType(std::string_view so) {
    static const char* kAssets[] = {"SoundBase", "SoundCue", "SoundWave", "MetaSoundSource", "AnimMontage", "AnimSequence", "AnimSequenceBase",
                                    "AnimationAsset", "BlendSpace", "StaticMesh", "SkeletalMesh", "MaterialInterface", "Material",
                                    "MaterialInstance", "MaterialInstanceConstant", "Texture", "Texture2D", "TextureCube", "ParticleSystem",
                                    "NiagaraSystem", "LevelSequence", "World", "DataAsset", "DataTable", "CurveFloat", "UserWidget", "Font"};
    for (auto* a : kAssets)
        if (so == a) return true;
    return false;
}
K baseKind(std::string_view cat, std::string_view so) {
    if (cat == "bool") return K::Bool;
    if (cat == "int" || cat == "byte" || cat == "enum") return K::Int;
    if (cat == "int64") return K::Int64;
    if (cat == "real" || cat == "float" || cat == "double") return K::Float;
    if (cat == "string") return K::String;
    if (cat == "name") return K::Name;
    if (cat == "text") return K::Text;
    if (cat == "struct") {
        if (so == "Vector" || so == "Vector3f" || so == "Vector3d" || so == "Vector2D" || so == "IntVector" || so == "Vector_NetQuantize" ||
            so == "Vector_NetQuantizeNormal")
            return K::Vector;
        if (so == "Rotator") return K::Rotator;
        if (so == "Transform") return K::Transform;
        if (so == "LinearColor" || so == "Color") return K::Color;
        if (so == "TimerHandle") return K::Timer;
        return K::Opaque;
    }
    if (cat == "object" || cat == "interface" || cat == "softobject" || cat == "weakobject")
        return isAssetType(so) ? K::Asset : K::Entity;
    if (cat == "class" || cat == "softclass") return K::Class;
    if (cat == "exec" || cat == "delegate" || cat == "mcdelegate" || cat.empty()) return K::Void;
    return K::Opaque;  // wildcard, fieldpath, ...
}
Ty tyOf(const PinType& t) {
    Ty r;
    r.k = baseKind(t.category, t.subObject);
    if (r.k == K::Void) return r;
    if (t.container == "Array") r.cont = 'A';
    else if (t.container == "Set") r.cont = 'S';
    else if (t.container == "Map") {
        r.cont = 'M';
        r.val = baseKind(t.valueCategory, t.valueSubObject);
        if (r.val == K::Void) r.val = K::Opaque;
    }
    return r;
}
std::string baseCpp(K k) {
    switch (k) {
        case K::Void: return "void";
        case K::Bool: return "bool";
        case K::Int: return "int";
        case K::Int64: return "long long";
        case K::Float: return "float";
        case K::String: case K::Name: case K::Text: case K::Class: case K::Asset: return "std::string";
        case K::Vector: return "Vec3";
        case K::Rotator: return "bp::Rotator";
        case K::Transform: return "bp::Transform";
        case K::Color: return "Color";
        case K::Entity: return "Entity";
        case K::Timer: return "TimerHandle";
        case K::Opaque: return "bp::Opaque";
    }
    return "bp::Opaque";
}
bool mapKeyOk(K k) { return k == K::Int || k == K::Int64 || k == K::String || k == K::Name || k == K::Text || k == K::Bool || k == K::Float; }
std::string cppOf(const Ty& t) {
    if (t.cont == 'A' || t.cont == 'S') return "std::vector<" + baseCpp(t.k) + ">";
    if (t.cont == 'M') return mapKeyOk(t.k) ? "std::map<" + baseCpp(t.k) + ", " + baseCpp(t.val) + ">" : "bp::Opaque";
    return baseCpp(t.k);
}
const char* kindName(K k) {
    switch (k) {
        case K::Void: return "void";
        case K::Bool: return "bool";
        case K::Int: return "int";
        case K::Int64: return "int64";
        case K::Float: return "float";
        case K::String: return "string";
        case K::Name: return "name";
        case K::Text: return "text";
        case K::Vector: return "vector";
        case K::Rotator: return "rotator";
        case K::Transform: return "transform";
        case K::Color: return "color";
        case K::Entity: return "entity";
        case K::Class: return "class";
        case K::Asset: return "asset";
        case K::Timer: return "timer";
        case K::Opaque: return "opaque";
    }
    return "?";
}
bool propertyOk(const Ty& t) {
    if (t.cont == 'M') return false;
    switch (t.k) {
        case K::Bool: case K::Int: case K::Float: case K::String: case K::Name: case K::Text: case K::Class: case K::Asset: case K::Vector:
        case K::Color: case K::Entity: return true;
        default: return false;
    }
}
Ty tyFromName(const std::string& n, const Ty& own) {
    std::string base = n;
    char cont = 0;
    for (auto [p, c] : {std::pair{"array:", 'A'}, std::pair{"set:", 'S'}, std::pair{"map:", 'M'}})
        if (startsWith(n, p)) {
            base = n.substr(std::string(p).size());
            cont = c;
        }
    if (base == "any") {
        Ty t = own;
        if (cont && t.cont != cont) return Ty{K::Void};  // signals a container mismatch
        return t;
    }
    static const std::map<std::string, K> m = {{"bool", K::Bool},       {"int", K::Int},       {"int64", K::Int64}, {"float", K::Float},
                                               {"string", K::String},   {"name", K::Name},     {"text", K::Text},   {"vector", K::Vector},
                                               {"rotator", K::Rotator}, {"transform", K::Transform}, {"entity", K::Entity}, {"class", K::Class},
                                               {"asset", K::Asset},     {"timer", K::Timer},   {"color", K::Color}};
    auto it = m.find(base);
    Ty t{it == m.end() ? K::Opaque : it->second, cont, K::Void};
    return t;
}
std::string defaultOf(const Ty& t) {
    if (t.k == K::Void) return "";
    return cppOf(t) + "{}";
}

// ---------------------------------------------------------------------------------------------- literals --
std::string floatLit(double v) {
    if (!std::isfinite(v)) v = 0;
    if (v > 3.0e38) v = 3.0e38;
    if (v < -3.0e38) v = -3.0e38;
    std::string s = std::format("{}", static_cast<float>(v));
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s + "f";
}
std::string intLit(long long v) {
    v = std::clamp(v, -2147483647LL, 2147483647LL);
    return std::to_string(v);
}
double parseNum(std::string_view s, bool* ok = nullptr) {
    std::string t(s);
    while (!t.empty() && (t.front() == ' ' || t.front() == '(')) t.erase(t.begin());
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (ok) *ok = end && end != t.c_str();
    if (!std::isfinite(v)) return 0;
    return v;
}
// "1.0,2.0,3.0" / "(X=1.0,Y=2.0,Z=3.0)" / "X=1 Y=2 Z=3" -> components by name (or by position)
std::vector<double> components(std::string_view s, std::initializer_list<const char*> names) {
    std::vector<double> out(names.size(), 0.0);
    std::string v(s);
    bool named = false;
    size_t i = 0;
    for (const char* n : names) {
        std::string key = std::string(n) + "=";
        size_t p = v.find(key);
        if (p != std::string::npos && (p == 0 || !std::isalpha(static_cast<unsigned char>(v[p - 1])))) {
            out[i] = parseNum(std::string_view(v).substr(p + key.size()));
            named = true;
        }
        ++i;
    }
    if (named) return out;
    std::string cleaned;
    for (char c : v) cleaned += (c == '(' || c == ')') ? ' ' : c;
    size_t start = 0;
    for (size_t k = 0; k < out.size(); ++k) {
        size_t comma = cleaned.find(',', start);
        out[k] = parseNum(std::string_view(cleaned).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}
std::string vecLit(const std::vector<double>& c) { return "Vec3(" + floatLit(c[0]) + ", " + floatLit(c[1]) + ", " + floatLit(c[2]) + ")"; }
std::string rotLit(double p, double y, double r) { return "bp::Rotator{" + floatLit(p) + ", " + floatLit(y) + ", " + floatLit(r) + "}"; }

std::string literalOf(const Ty& t, const std::string& value, const std::string& object, const std::string& text) {
    if (t.cont) return defaultOf(t);
    switch (t.k) {
        case K::Void: return "";
        case K::Bool: return toLower(value) == "true" || value == "1" ? "true" : "false";
        case K::Int:
        case K::Int64: {
            bool ok = false;
            double v = parseNum(value, &ok);
            if (!ok && !value.empty()) {  // enum entry "NewEnumerator2" -> 2
                size_t d = value.find_last_not_of("0123456789");
                v = d + 1 < value.size() ? parseNum(std::string_view(value).substr(d + 1)) : 0;
            }
            return intLit(static_cast<long long>(v));
        }
        case K::Float: return floatLit(parseNum(value));
        case K::String:
        case K::Name: return "std::string(" + cppStringLiteral(t.k == K::Name && value == "None" ? "" : value) + ")";
        case K::Text: return "std::string(" + cppStringLiteral(text.empty() ? value : text) + ")";
        case K::Class:
        case K::Asset: return "std::string(" + cppStringLiteral(object.empty() ? value : object) + ")";
        case K::Vector: return value.empty() ? "Vec3()" : vecLit(components(value, {"X", "Y", "Z"}));
        case K::Rotator: {
            if (value.empty()) return "bp::Rotator{}";
            if (value.find("Pitch") != std::string::npos || value.find("P=") != std::string::npos) {
                auto c = components(value, {"Pitch", "Yaw", "Roll"});
                if (value.find("Pitch") == std::string::npos) c = components(value, {"P", "Y", "R"});
                return rotLit(c[0], c[1], c[2]);
            }
            auto c = components(value, {"_p", "_y", "_r"});  // positional: Pitch,Yaw,Roll
            return rotLit(c[0], c[1], c[2]);
        }
        case K::Transform: {
            if (value.empty()) return "bp::Transform{}";
            auto grab = [&](const char* key) -> std::string {
                size_t p = value.find(std::string(key) + "=(");
                if (p == std::string::npos) return {};
                size_t e = value.find(')', p);
                return value.substr(p + std::string(key).size() + 1, e == std::string::npos ? std::string::npos : e - p - std::string(key).size());
            };
            std::string tr = grab("Translation"), sc = grab("Scale3D");
            return "bp::Transform{" + (tr.empty() ? std::string("Vec3()") : vecLit(components(tr, {"X", "Y", "Z"}))) + ", bp::Rotator{}, " +
                   (sc.empty() ? std::string("Vec3(1.0f, 1.0f, 1.0f)") : vecLit(components(sc, {"X", "Y", "Z"}))) + "}";
        }
        case K::Color: {
            if (value.empty()) return "Color{}";
            auto c = components(value, {"R", "G", "B"});
            return "Color{" + floatLit(c[0]) + ", " + floatLit(c[1]) + ", " + floatLit(c[2]) + "}";
        }
        case K::Entity: return "Entity()";
        case K::Timer: return "TimerHandle{}";
        case K::Opaque: return "bp::Opaque{}";
    }
    return "{}";
}

struct Expr {
    std::string code;
    Ty ty;
    bool lvalue = false;
};

std::string stripK2(std::string s) {
    if (startsWith(s, "K2_")) s = s.substr(3);
    return s;
}
std::string lowerStr(std::string_view s) { return toLower(s); }
bool isExecPin(const Pin& p) { return p.type.category == "exec"; }
bool isDataPin(const Pin& p) {
    return p.type.category != "exec" && p.type.category != "delegate" && p.type.category != "mcdelegate";
}

// ============================================================================================ emitter ==
class Emitter {
public:
    Emitter(const Graph& g, const Options& o, const MappingTable& m) : g(g), opt(o), map(m) {
        for (auto& n : g.nodes) byName.emplace(n.graph + "\x1f" + n.name, &n);  // names are unique per graph
        for (auto& n : g.nodes)
            for (auto& p : n.pins) owner[&p] = &n;
    }

    Conversion run(const std::string& className) {
        Conversion c;
        generate(className);  // pass 1: discovers which event parameters other flows read (mirrors), subscriptions, ...
        generate(className);  // pass 2: final text
        c.report = rep;
        c.className = className;
        if (!fatal.empty()) {
            c.report.error = fatal;
            return c;
        }
        c.cppFile = className + ".cpp";
        c.cpp = cppText;
        if (!headerText.empty()) {
            c.headerFile = className + ".h";
            c.header = headerText;
        }
        c.report.files.push_back(c.cppFile);
        if (!c.headerFile.empty()) c.report.files.push_back(c.headerFile);
        c.ok = true;
        return c;
    }

private:
    const Graph& g;
    const Options& opt;
    const MappingTable& map;
    std::map<std::string, const Node*> byName;
    std::map<const Pin*, const Node*> owner;

    struct PinRef {
        const Node* n = nullptr;
        const Pin* p = nullptr;
    };
    struct Param {
        std::string ue, name;
        Ty ty;
        const Pin* pin = nullptr;
    };
    struct Root {
        const Node* node = nullptr;
        const Pin* start = nullptr;
        std::string kind;  // event | custom | function | input
        std::string fn, callback, title;
        const EventMapping* em = nullptr;
        bool latent = false, isPublic = false;
        std::vector<Param> params, outs;
        std::string dispatch;  // input polling line for OnUpdate
        Severity sev = Severity::Converted;
        std::string sevNote;
    };
    struct Var {
        std::string ue, cpp;
        Ty ty;
        std::string def, tooltip, unrealType;
        bool declared = false;
    };
    struct Member {
        std::string type, name, init, comment;
    };
    struct Dispatcher {
        std::string ue, structName;
        std::vector<std::pair<std::string, Ty>> fields;
    };
    struct Ctx {
        const Root* root = nullptr;  // null in helpers / sub-flows (event parameters are then read from mirrors)
        const Root* fnRoot = nullptr;  // the function whose FunctionResult nodes return
        bool latent = false, noHelpers = false;
        int depth = 0;
        std::vector<const Node*> path, loops;
    };
    struct Out {
        std::string s;
        int ind = 2;
        void line(const std::string& l) { s += std::string(static_cast<size_t>(ind) * 4, ' ') + l + "\n"; }
    };

    // ---- per-pass state
    Report rep;
    std::string fatal, cppText, headerText, cls;
    std::set<std::string> used;
    std::vector<NameEntry> names;
    std::map<std::string, Var> vars;  // by Unreal name
    std::vector<std::string> varOrder;
    std::vector<Member> members;
    std::map<std::string, std::string> memberByKey, memberTypes;
    std::map<std::string, Ty> memberTys;
    std::vector<Root> roots;
    std::map<std::string, size_t> rootByName;  // lower(custom event / function name)
    std::map<std::string, Dispatcher> dispatchers;
    std::vector<std::string> publicFns, privateFns, helperFns;
    std::map<std::string, std::string> helperByKey, stubFnByName;
    std::map<std::string, std::vector<std::string>> callbacks;
    std::vector<std::string> startPrologue;
    struct NodeState {
        Severity sev = Severity::Ignored;
        bool touched = false;
        std::string detail;
    };
    std::map<const Node*, NodeState> nodeState;
    std::set<std::string> unrecognisedSeen;
    size_t statements = 0, evaluations = 0;
    int subflowCounter = 0;
    bool usesMovement = false;
    std::vector<const Node*> evalStack;
    // ---- persistent across passes
    std::set<const Pin*> mirrorNeeded;
    std::map<std::string, std::vector<std::string>> subsByDispatcher;  // dispatcher -> subscription members

    // ------------------------------------------------------------------------------------------ utils --
    std::string claim(std::string_view text, std::string_view fallback = "Value") {
        std::string base = sanitizeIdentifier(text, fallback);
        std::string n = base;
        for (int i = 2; used.contains(n) || n == cls; ++i) n = base + "_" + std::to_string(i);
        used.insert(n);
        return n;
    }
    std::string title(const Node& n) const {
        const std::string& c = n.cls;
        if (c == "K2Node_CallFunction" || c == "K2Node_CallArrayFunction" || c == "K2Node_CallParentFunction") return stripK2(n.memberName);
        if (c == "K2Node_VariableGet") return "Get " + n.memberName;
        if (c == "K2Node_VariableSet") return "Set " + n.memberName;
        if (c == "K2Node_Event") return "Event " + n.memberName;
        if (c == "K2Node_CustomEvent") return "Custom Event " + n.memberName;
        if (c == "K2Node_FunctionEntry") return "Function " + n.memberName;
        if (c == "K2Node_FunctionResult") return "Return " + n.memberName;
        if (c == "K2Node_MacroInstance") return n.memberName.empty() ? "Macro" : n.memberName;
        if (c == "K2Node_IfThenElse") return "Branch";
        if (c == "K2Node_ExecutionSequence") return "Sequence";
        if (c == "K2Node_DynamicCast") return "Cast To " + n.memberName;
        if (c == "K2Node_InputAction") return "InputAction " + n.memberName;
        if (c == "K2Node_InputAxisEvent") return "InputAxis " + n.memberName;
        if (c == "K2Node_InputKey") return "InputKey " + n.memberName;
        if (c == "K2Node_EnhancedInputAction") return "EnhancedInputAction " + n.memberName;
        if (c == "K2Node_CallDelegate") return "Call " + n.memberName;
        if (c == "K2Node_AddDelegate" || c == "K2Node_AssignDelegate") return "Bind " + n.memberName;
        std::string s = c;
        if (startsWith(s, "K2Node_")) s = s.substr(7);
        return s;
    }
    std::string shortTitle(const Node& n) const {  // for member names
        std::string t = title(n);
        for (const char* p : {"Get ", "Set ", "Event ", "Custom Event ", "Function ", "Cast To "})
            if (startsWith(t, p)) t = t.substr(std::string(p).size());
        return t;
    }
    void mark(const Node& n, Severity s, std::string detail = {}) {
        NodeState& st = nodeState[&n];
        auto rank = [](Severity x) { return x == Severity::Ignored ? -1 : static_cast<int>(x); };
        if (!st.touched || rank(s) > rank(st.sev)) {
            st.sev = s;
            if (!detail.empty()) st.detail = detail;
        } else if (st.detail.empty() && !detail.empty() && s == st.sev) {
            st.detail = detail;
        }
        st.touched = true;
    }
    void unrecognised(const std::string& construct, const std::string& name, const Node* n, const std::string& detail) {
        std::string key = construct + "|" + name;
        if (!unrecognisedSeen.insert(key).second) return;
        rep.unrecognised.push_back({construct, name.substr(0, 200), n ? n->name.substr(0, 120) : std::string(), detail.substr(0, 300), n ? n->line : 0});
    }
    void warn(const std::string& w) {
        if (rep.warnings.size() < 200 && std::find(rep.warnings.begin(), rep.warnings.end(), w) == rep.warnings.end()) rep.warnings.push_back(w);
    }
    std::vector<PinRef> linksOf(const Pin* p) const {
        std::vector<PinRef> out;
        if (!p) return out;
        for (auto& l : p->links) {
            auto on = owner.find(p);
            auto it = byName.find((on == owner.end() ? std::string() : on->second->graph) + "\x1f" + l.node);
            if (it == byName.end()) continue;
            const Pin* tp = it->second->pinById(l.pinId);
            if (tp) out.push_back({it->second, tp});
        }
        return out;
    }
    static const Pin* execOut(const Node& n, std::string_view name) {
        for (auto& p : n.pins)
            if (p.dir == PinDir::Out && isExecPin(p) && p.name == name) return &p;
        return nullptr;
    }
    static std::vector<const Pin*> execOuts(const Node& n) {
        std::vector<const Pin*> v;
        for (auto& p : n.pins)
            if (p.dir == PinDir::Out && isExecPin(p)) v.push_back(&p);
        return v;
    }
    static const Pin* firstExecOut(const Node& n) {
        if (auto* p = execOut(n, "then")) return p;
        auto v = execOuts(n);
        return v.size() == 1 ? v[0] : nullptr;
    }
    static const Pin* dataIn(const Node& n, std::string_view name) {
        for (auto& p : n.pins)
            if (p.dir == PinDir::In && p.name == name && isDataPin(p)) return &p;
        return nullptr;
    }
    std::string member(const std::string& key, const std::string& type, const std::string& base, const std::string& init = {}, const std::string& comment = {},
                       std::optional<Ty> ty = std::nullopt) {
        auto it = memberByKey.find(key);
        if (it != memberByKey.end()) return it->second;
        std::string n = claim("m_" + sanitizeIdentifier(base, "value"), "m_value");
        members.push_back({type, n, init, comment});
        memberByKey[key] = n;
        memberTypes[n] = type;
        if (ty) memberTys[n] = *ty;
        return n;
    }
    std::string outMember(const Node& n, const Pin& p, std::optional<Ty> forced = std::nullopt) {
        Ty t = forced ? *forced : tyOf(p.type);
        return member(n.name + "#" + p.id, cppOf(t), shortTitle(n) + "_" + p.name, "", "output '" + p.name + "' of " + title(n), t);
    }
    // "member = value;" with the value converted to the member's declared type (hostile pin types never break the build)
    std::string assign(const std::string& m, const std::string& rhs) {
        auto it = memberTypes.find(m);
        return m + " = " + (it == memberTypes.end() ? rhs : "bp::As<" + it->second + ">(" + rhs + ")") + ";";
    }
    std::string coerce(const Expr& e, const Ty& want) {
        if (want.k == K::Void || e.code.empty()) return e.code.empty() ? defaultOf(want) : e.code;
        if (cppOf(e.ty) == cppOf(want)) return e.code;
        return "bp::As<" + cppOf(want) + ">(" + e.code + ")";
    }
    std::string stubValue(const Ty& t) { return t.k == K::Void ? std::string("0") : "bp::Stub<" + cppOf(t) + ">()"; }

    // ------------------------------------------------------------------------------------- variables --
    Var& variable(const std::string& ue, const PinType* pt) {
        auto it = vars.find(ue);
        if (it != vars.end()) {
            if (pt && it->second.ty.k == K::Opaque && tyOf(*pt).k != K::Opaque && !it->second.declared) it->second.ty = tyOf(*pt);
            return it->second;
        }
        Var v;
        v.ue = ue;
        v.ty = pt ? tyOf(*pt) : Ty{K::Opaque};
        if (v.ty.k == K::Void) v.ty = Ty{K::Opaque};
        if (pt) v.unrealType = pt->category + (pt->subObject.empty() ? "" : ":" + pt->subObject) + (pt->container.empty() ? "" : " " + pt->container);
        v.cpp = claim(ue, "Variable");
        names.push_back({ue, v.cpp, "variable"});
        varOrder.push_back(ue);
        return vars.emplace(ue, std::move(v)).first->second;
    }

    // ----------------------------------------------------------------------------------- expressions --
    Expr inputExpr(const Node& n, const Pin* in, Ctx& c, int depth) {
        if (!in) return {"", Ty{}};
        Ty own = tyOf(in->type);
        auto links = linksOf(in);
        if (!links.empty()) return value(*links[0].n, *links[0].p, c, depth + 1);
        if (!in->subPins.empty() && in->hidden) {  // split struct pin: rebuild from the sub-pins
            std::vector<std::string> parts;
            for (auto& id : in->subPins)
                if (const Pin* sp = n.pinById(id)) parts.push_back(coerce(inputExpr(n, sp, c, depth + 1), Ty{K::Float}));
            if (own.k == K::Vector && parts.size() == 3) return {"Vec3(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ")", own};
            if (own.k == K::Rotator && parts.size() == 3) return {"bp::Rotator{" + parts[1] + ", " + parts[2] + ", " + parts[0] + "}", own};  // Roll, Pitch, Yaw
        }
        return {literalOf(own, in->defaultValue, in->defaultObject, in->defaultText), own};
    }
    std::string arg(const Node& n, std::string_view pinName, const Ty& want, Ctx& c, int depth) {
        const Pin* p = dataIn(n, pinName);
        if (!p) return want.k == K::Void ? std::string() : defaultOf(want);
        return coerce(inputExpr(n, p, c, depth), want);
    }
    Expr param(const Node& n, const Pin& p, Ctx& c) {
        if (c.root && c.root->node == &n)
            for (auto& pa : c.root->params)
                if (pa.pin == &p) return {pa.name, pa.ty, false};
        for (auto& r : roots)
            if (r.node == &n)
                for (auto& pa : r.params)
                    if (pa.pin == &p) {
                        if (c.root && c.root->node == &n) return {pa.name, pa.ty, false};
                        mirrorNeeded.insert(&p);
                        return {member("mirror#" + n.name + "#" + p.id, cppOf(pa.ty), shortTitle(n) + "_" + p.name, "", "copy of event parameter '" + p.name + "'"), pa.ty, true};
                    }
        return {defaultOf(tyOf(p.type)), tyOf(p.type)};
    }
    bool isRootNode(const Node& n) const {
        for (auto& r : roots)
            if (r.node == &n) return true;
        return false;
    }

    Expr value(const Node& n, const Pin& out, Ctx& c, int depth) {
        Ty own = tyOf(out.type);
        if (++evaluations > 2000000) {
            if (fatal.empty()) fatal = "the graph is too complex to convert (expression evaluation limit reached)";
            return {stubValue(own), own};
        }
        if (depth > opt.limits.maxExpressionDepth) {
            warn("expression nesting deeper than " + std::to_string(opt.limits.maxExpressionDepth) + " nodes: the rest was stubbed");
            mark(n, Severity::Stubbed, "expression too deep");
            return {stubValue(own), own};
        }
        if (std::find(evalStack.begin(), evalStack.end(), &n) != evalStack.end()) {
            mark(n, Severity::Stubbed, "data cycle");
            warn("a data cycle through " + commentText(title(n), 60) + " was cut with a stub");
            return {stubValue(own), own};
        }
        evalStack.push_back(&n);
        Expr e = valueImpl(n, out, c, depth);
        evalStack.pop_back();
        if (e.code.size() > 200000) {
            warn("an expression grew beyond 200 KB and was stubbed");
            return {stubValue(own), own};
        }
        return e;
    }

    Expr selfOf(const Node& n, Ctx& c, int depth) {
        const Pin* self = dataIn(n, "self");
        if (self && !linksOf(self).empty()) return {coerce(inputExpr(n, self, c, depth), Ty{K::Entity}), Ty{K::Entity}};
        return {"GetEntity()", Ty{K::Entity}};
    }

    // ${...} template expansion; sets `failed` when a container argument does not have the right container type
    std::string expand(const std::string& tpl, const Node& n, const FunctionMapping& fm, Ctx& c, int depth, bool& failed) {
        std::string out;
        size_t i = 0;
        while (i < tpl.size()) {
            size_t s = tpl.find("${", i);
            if (s == std::string::npos) {
                out += tpl.substr(i);
                break;
            }
            out += tpl.substr(i, s - i);
            size_t e = tpl.find('}', s);
            if (e == std::string::npos) break;
            std::string tok = tpl.substr(s + 2, e - s - 2);
            i = e + 1;
            if (tok == "self") {
                out += selfOf(n, c, depth).code;
                continue;
            }
            if (startsWith(tok, "member:")) {
                std::string pn = tok.substr(7);
                const Pin* p = n.pin(pn, PinDir::Out);
                out += p ? outMember(n, *p) : member(n.name + "#unused#" + pn, "int", shortTitle(n) + "_" + pn, "", "unused output");
                continue;
            }
            bool ref = startsWith(tok, "ref:");
            std::string pn = ref ? tok.substr(4) : tok;
            const Pin* p = dataIn(n, pn);
            if (!p)
                if (auto a = fm.pinAliases.find(pn); a != fm.pinAliases.end()) p = dataIn(n, a->second);
            std::string tn = fm.args.contains(pn) ? fm.args.at(pn) : "any";
            Ty own = p ? tyOf(p->type) : Ty{K::Opaque};
            Ty want = tyFromName(tn, own);
            if (want.k == K::Void) {  // container mismatch ("array:any" on a non-array pin)
                failed = true;
                out += "0";
                continue;
            }
            if (!p) {
                out += defaultOf(want.k == K::Opaque && tn == "any" ? Ty{K::Opaque} : want);
                continue;
            }
            Expr ex = inputExpr(n, p, c, depth);
            if ((tn.find(':') != std::string::npos) && ex.ty.cont != want.cont) {
                failed = true;
                out += "0";
                continue;
            }
            if (ref) {
                if (ex.lvalue && cppOf(ex.ty) == cppOf(want)) out += ex.code;
                else out += "(" + member(n.name + "#scratch#" + pn, cppOf(want), shortTitle(n) + "_" + pn + "_tmp", "", "scratch copy") + " = " + coerce(ex, want) + ")";
            } else {
                out += coerce(ex, want);
            }
        }
        return out;
    }

    // function / custom-event roots by name (self calls)
    const Root* selfTarget(const Node& n) const {
        if (!n.selfContext && !n.memberParent.empty() && n.memberParent != g.blueprintName + "_C" && n.memberParent != g.blueprintName) return nullptr;
        auto it = rootByName.find(lowerStr(n.memberName));
        return it == rootByName.end() ? nullptr : &roots[it->second];
    }
    std::string callArgs(const Node& n, const Root& r, Ctx& c, int depth) {
        std::string a;
        for (auto& pa : r.params) {
            if (!a.empty()) a += ", ";
            a += arg(n, pa.ue, pa.ty, c, depth);
        }
        return a;
    }
    // A self call to a function that is not in the input: a typed stub member function keeps the file compiling.
    std::string stubFunction(const Node& n, Ctx& c, int depth, std::string& argsOut, std::vector<const Pin*>& outsOut, Ty& retOut) {
        std::vector<const Pin*> ins;
        outsOut.clear();
        for (auto& p : n.pins) {
            if (!isDataPin(p) || p.name == "self") continue;
            (p.dir == PinDir::In ? ins : outsOut).push_back(&p);
        }
        retOut = outsOut.size() == 1 ? tyOf(outsOut[0]->type) : Ty{K::Void};
        argsOut.clear();
        for (auto* p : ins) argsOut += (argsOut.empty() ? "" : ", ") + coerce(inputExpr(n, p, c, depth), tyOf(p->type));
        std::string key = lowerStr(n.memberName);
        auto it = stubFnByName.find(key);
        if (it != stubFnByName.end()) return it->second;
        std::string fn = claim(n.memberName, "Function");
        names.push_back({n.memberName, fn, "function (stub)"});
        std::string sig, body;
        int k = 0;
        for (auto* p : ins) sig += (sig.empty() ? "" : ", ") + std::string("[[maybe_unused]] ") + cppOf(tyOf(p->type)) + " a" + std::to_string(k++);
        if (outsOut.size() > 1)
            for (auto* p : outsOut) sig += (sig.empty() ? "" : ", ") + cppOf(tyOf(p->type)) + "& out" + std::to_string(k++);
        std::string ret = outsOut.size() == 1 ? cppOf(retOut) : "void";
        std::string f = "    // TODO(blueprint): '" + commentText(n.memberName, 80) + "' is called but was not in the converted text - implement it.\n";
        f += "    " + ret + " " + fn + "(" + sig + ") {\n";
        if (ret != "void") f += "        return " + defaultOf(retOut) + ";\n";
        f += "    }\n";
        publicFns.push_back(f);
        stubFnByName[key] = fn;
        unrecognised("function", n.memberName, &n, "self call to a function that is not in the input; a stub member function was generated");
        return fn;
    }

    Expr valueImpl(const Node& n, const Pin& out, Ctx& c, int depth) {
        Ty own = tyOf(out.type);
        const std::string& k = n.cls;
        if (isRootNode(n)) {
            mark(n, Severity::Converted);
            return param(n, out, c);
        }
        if (k == "K2Node_Knot") {
            for (auto& p : n.pins)
                if (p.dir == PinDir::In) return inputExpr(n, &p, c, depth);
            return {defaultOf(own), own};
        }
        if (k == "K2Node_VariableGet") {
            const Pin* self = dataIn(n, "self");
            if (self && !linksOf(self).empty() && !n.selfContext) {
                mark(n, Severity::Stubbed, "variable of another object");
                unrecognised("variable on another object", n.memberName, &n, "reading a variable of another Blueprint is not converted");
                return {"bp::Stub<" + cppOf(own) + ">() /* TODO(blueprint): " + commentText(n.memberName, 60) + " of another object */", own};
            }
            Var& v = variable(n.memberName, &out.type);
            mark(n, Severity::Converted);
            return {v.cpp, v.ty, true};
        }
        if (k == "K2Node_TemporaryVariable") {
            mark(n, Severity::Approximated, "local variable stored in a member");
            return {member(n.name + "#temp", cppOf(own), "Local_" + out.name), own, true};
        }
        if (k == "K2Node_Self") {
            mark(n, Severity::Converted);
            return {"GetEntity()", Ty{K::Entity}};
        }
        if (k == "K2Node_VariableSet") {  // Output_Get
            mark(n, Severity::Converted);
            Var& v = variable(n.memberName, &out.type);
            return {v.cpp, v.ty, true};
        }
        if (k == "K2Node_MacroInstance") {
            std::string m = lowerStr(n.memberName);
            if (m == "don" && out.name == "Counter") return {member(n.name + "#state", "bp::DoN", "DoN") + ".counter", Ty{K::Int}};
            if (m == "flipflop" && out.name == "IsA") return {member(n.name + "#state", "bp::FlipFlop", "FlipFlop") + ".isA", Ty{K::Bool}};
            return {member(n.name + "#" + out.name, cppOf(own), shortTitle(n) + "_" + out.name), own, true};
        }
        if (k == "K2Node_DynamicCast") {
            const Pin* obj = dataIn(n, "Object");
            if (n.hasExecInput()) {
                std::string m = member(n.name + "#as", "Entity", "As" + n.memberName);
                if (out.name == "bSuccess") return {m + ".IsValid()", Ty{K::Bool}};
                return {m, Ty{K::Entity}, true};
            }
            mark(n, Severity::Approximated, "Fledge has no actor classes: any valid entity passes the cast");
            std::string o = obj ? coerce(inputExpr(n, obj, c, depth), Ty{K::Entity}) : std::string("Entity()");
            if (out.name == "bSuccess") return {"(" + o + ").IsValid()", Ty{K::Bool}};
            return {o, Ty{K::Entity}};
        }
        if (k == "K2Node_Timeline") {
            mark(n, Severity::Stubbed, "timelines are not converted");
            return {member(n.name + "#" + out.name, cppOf(own), shortTitle(n) + "_" + out.name), own, true};
        }
        if (n.hasExecInput()) {  // impure node: its outputs were stored when it ran
            std::optional<Ty> forced;
            if ((k == "K2Node_CallFunction" || k == "K2Node_CallArrayFunction") && !n.memberName.empty())
                if (const Root* r = selfTarget(n); r && r->kind == "function")
                    for (auto& o : r->outs)
                        if (o.ue == out.name) forced = o.ty;
            std::string m = outMember(n, out, forced);
            auto it = memberTys.find(m);
            return {m, it == memberTys.end() ? own : it->second, true};
        }
        if (k == "K2Node_CallFunction" || k == "K2Node_CallArrayFunction" || k == "K2Node_CommutativeAssociativeBinaryOperator" ||
            k == "K2Node_PromotableOperator") {
            if (const Root* r = selfTarget(n); r && r->kind != "function") {
                mark(n, Severity::Stubbed, "a custom event used as a pure value");
                return {stubValue(own), own};
            }
            if (const Root* r = selfTarget(n)) {
                mark(n, Severity::Converted);
                std::string a = callArgs(n, *r, c, depth);
                if (r->outs.size() <= 1) return {r->fn + "(" + a + ")", r->outs.empty() ? own : r->outs[0].ty};
                std::string extra, pick;
                Ty pickTy = own;
                for (auto& o : r->outs) {
                    const Pin* cp = n.pin(o.ue, PinDir::Out);
                    std::string m = cp ? outMember(n, *cp, o.ty) : member(n.name + "#" + o.ue, cppOf(o.ty), shortTitle(n) + "_" + o.ue, "", "", o.ty);
                    extra += ", " + m;
                    if (cp == &out) {
                        pick = m;
                        pickTy = memberTys.contains(m) ? memberTys[m] : o.ty;
                    }
                }
                if (pick.empty()) pick = defaultOf(own);
                return {"(" + r->fn + "(" + a + (a.empty() ? extra.substr(2) : extra) + "), " + pick + ")", pickTy};
            }
            const FunctionMapping* fm = map.function(n.memberName, n.memberParent);
            if (!fm || (!fm->stmt.empty() && fm->expr.empty() && fm->outputs.empty() && fm->op.empty()) || !fm->handler.empty()) {
                if (n.selfContext && !fm) {
                    std::string a;
                    std::vector<const Pin*> outs;
                    Ty ret;
                    std::string fn = stubFunction(n, c, depth, a, outs, ret);
                    mark(n, Severity::Stubbed, "function not in the input");
                    if (outs.size() == 1) return {fn + "(" + a + ")", ret};
                    return {stubValue(own), own};
                }
                mark(n, Severity::Stubbed, fm ? "mapping is not usable as a pure expression" : "no Fledge equivalent");
                if (!fm) unrecognised("function", (n.memberParent.empty() ? "" : n.memberParent + "::") + n.memberName, &n, "no mapping (pure)");
                return {"bp::Stub<" + cppOf(own) + ">() /* TODO(blueprint): " + commentText(stripK2(n.memberName), 60) + " - no Fledge equivalent */", own};
            }
            bool failed = false;
            Expr r;
            if (!fm->op.empty() && out.name == "ReturnValue") {
                Ty want = fm->args.contains("A") ? tyFromName(fm->args.at("A"), own) : own;
                std::string acc;
                for (auto& p : n.pins) {
                    if (p.dir != PinDir::In || !isDataPin(p) || p.name == "self") continue;
                    std::string a = coerce(inputExpr(n, &p, c, depth), want);
                    acc = acc.empty() ? a : acc + " " + fm->op + " " + a;
                }
                if (acc.empty()) acc = defaultOf(want);
                r = {"(" + acc + ")", want};
                if (fm->outTypes.contains("ReturnValue")) r.ty = tyFromName(fm->outTypes.at("ReturnValue"), own);
            } else if (out.name == "ReturnValue" && !fm->expr.empty()) {
                r = {expand(fm->expr, n, *fm, c, depth, failed), fm->outTypes.contains("ReturnValue") ? tyFromName(fm->outTypes.at("ReturnValue"), own) : own};
            } else if (auto it = fm->outputs.find(out.name); it != fm->outputs.end()) {
                r = {expand(it->second, n, *fm, c, depth, failed), fm->outTypes.contains(out.name) ? tyFromName(fm->outTypes.at(out.name), own) : own};
            } else {
                mark(n, Severity::Approximated, "output '" + out.name + "' is not mapped");
                return {defaultOf(own), own};
            }
            if (failed) {
                mark(n, Severity::Stubbed, "argument types do not match the mapping (container expected)");
                return {stubValue(own), own};
            }
            if (r.ty.k == K::Void || r.ty.k == K::Opaque) r.ty = own;
            mark(n, fm->status == "approximated" ? Severity::Approximated : fm->status == "stubbed" ? Severity::Stubbed : Severity::Converted, fm->note);
            return r;
        }
        if (k == "K2Node_MakeStruct" || k == "K2Node_BreakStruct") {
            bool make = k == "K2Node_MakeStruct";
            const std::string& s = n.memberName;
            auto f = [&](const char* pn) { return arg(n, pn, Ty{K::Float}, c, depth); };
            if (make) {
                mark(n, Severity::Converted);
                if (own.k == K::Vector) return {"Vec3(" + f("X") + ", " + f("Y") + ", " + f("Z") + ")", own};
                if (own.k == K::Rotator) return {"bp::Rotator{" + f("Pitch") + ", " + f("Yaw") + ", " + f("Roll") + "}", own};
                if (own.k == K::Color) return {"Color{" + f("R") + ", " + f("G") + ", " + f("B") + "}", own};
                if (own.k == K::Transform)
                    return {"bp::Transform{" + arg(n, "Location", Ty{K::Vector}, c, depth) + ", " + arg(n, "Rotation", Ty{K::Rotator}, c, depth) + ", " +
                                arg(n, "Scale", Ty{K::Vector}, c, depth) + "}",
                            own};
            } else {
                const Pin* in = nullptr;
                for (auto& p : n.pins)
                    if (p.dir == PinDir::In && isDataPin(p)) in = &p;
                if (in) {
                    Expr src = inputExpr(n, in, c, depth);
                    Ty st = tyOf(in->type);
                    std::string x = "(" + coerce(src, st) + ")";
                    static const std::map<std::string, std::pair<std::string, K>> fields = {
                        {"X", {".x", K::Float}},          {"Y", {".y", K::Float}},          {"Z", {".z", K::Float}},
                        {"Roll", {".roll", K::Float}},    {"Pitch", {".pitch", K::Float}},  {"Yaw", {".yaw", K::Float}},
                        {"R", {".r", K::Float}},          {"G", {".g", K::Float}},          {"B", {".b", K::Float}},
                        {"Location", {".location", K::Vector}}, {"Rotation", {".rotation", K::Rotator}}, {"Scale", {".scale", K::Vector}},
                        {"Translation", {".location", K::Vector}}};
                    if (st.k == K::Vector || st.k == K::Rotator || st.k == K::Color || st.k == K::Transform)
                        if (auto it = fields.find(out.name); it != fields.end()) {
                            mark(n, Severity::Converted);
                            return {x + it->second.first, Ty{it->second.second}};
                        }
                }
            }
            mark(n, Severity::Stubbed, "struct '" + s + "' is not supported");
            unrecognised("struct", s, &n, make ? "Make struct" : "Break struct");
            return {stubValue(own), own};
        }
        if (k == "K2Node_MakeArray") {
            Ty elem = own;
            elem.cont = 0;
            std::string items;
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && isDataPin(p)) items += (items.empty() ? "" : ", ") + coerce(inputExpr(n, &p, c, depth), elem);
            mark(n, Severity::Converted);
            Ty arr = own;
            if (!arr.cont) arr.cont = 'A';
            return {cppOf(arr) + "{" + items + "}", arr};
        }
        if (k == "K2Node_Select") {
            const Pin* idx = dataIn(n, "Index");
            std::vector<std::string> opts;
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && isDataPin(p) && &p != idx) opts.push_back(coerce(inputExpr(n, &p, c, depth), own));
            mark(n, Severity::Converted);
            if (!idx) return {opts.empty() ? defaultOf(own) : opts[0], own};
            Expr ie = inputExpr(n, idx, c, depth);
            if (tyOf(idx->type).k == K::Bool && opts.size() == 2) return {"(" + coerce(ie, Ty{K::Bool}) + " ? " + opts[1] + " : " + opts[0] + ")", own};
            std::string list;
            for (auto& o : opts) list += (list.empty() ? "" : ", ") + o;
            return {"bp::Select<" + cppOf(own) + ">(" + coerce(ie, Ty{K::Int}) + ", {" + list + "})", own};
        }
        if (k == "K2Node_GetArrayItem") {
            const Pin *arr = nullptr, *ix = nullptr;
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && isDataPin(p)) {
                    if (!p.type.container.empty() && !arr) arr = &p;
                    else if (!ix) ix = &p;
                }
            if (arr) {
                Expr a = inputExpr(n, arr, c, depth);
                if (a.ty.cont == 'A' || a.ty.cont == 'S') {
                    mark(n, Severity::Converted);
                    Ty el = a.ty;
                    el.cont = 0;
                    return {"bp::ArrayGet(" + a.code + ", " + (ix ? coerce(inputExpr(n, ix, c, depth), Ty{K::Int}) : std::string("0")) + ")", el};
                }
            }
            mark(n, Severity::Stubbed, "array input missing");
            return {stubValue(own), own};
        }
        if (k == "K2Node_EnumLiteral") {
            mark(n, Severity::Approximated, "enums are ints");
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && isDataPin(p)) return {literalOf(Ty{K::Int}, p.defaultValue, "", ""), Ty{K::Int}};
            return {"0", Ty{K::Int}};
        }
        if (k == "K2Node_FormatText") {
            const Pin* fmt = dataIn(n, "Format");
            std::string pattern = fmt ? literalOf(Ty{K::Text}, fmt->defaultValue, "", fmt->defaultText) : std::string("std::string()");
            if (fmt && !linksOf(fmt).empty()) pattern = coerce(inputExpr(n, fmt, c, depth), Ty{K::String});
            std::string a;
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && isDataPin(p) && &p != fmt)
                    a += (a.empty() ? "" : ", ") + std::string("{") + cppStringLiteral(p.name) + ", " + coerce(inputExpr(n, &p, c, depth), Ty{K::String}) + "}";
            mark(n, Severity::Approximated, "argument names are replaced literally; no number formatting");
            return {"bp::Format(" + pattern + ", {" + a + "})", Ty{K::Text}};
        }
        if (k == "K2Node_CreateDelegate") {
            mark(n, Severity::Converted);
            return {"0", Ty{K::Int}};
        }
        mark(n, Severity::Stubbed, "node class not supported");
        unrecognised("node class", k, &n, "pure node");
        return {"bp::Stub<" + cppOf(own) + ">() /* TODO(blueprint): " + commentText(title(n), 60) + " */", own};
    }

    // ----------------------------------------------------------------------------------------- exec --
    bool isLatentNode(const Node& n) const {
        if (n.cls != "K2Node_CallFunction") return false;
        const FunctionMapping* fm = map.function(n.memberName, n.memberParent);
        return fm && (fm->handler == "delay" || fm->handler == "retriggerable_delay" || fm->handler == "next_tick");
    }
    bool reachesLatent(const Pin* start) const {
        std::vector<const Pin*> todo{start};
        std::set<const Node*> seen;
        size_t guard = 0;
        while (!todo.empty() && guard++ < 200000) {
            const Pin* p = todo.back();
            todo.pop_back();
            for (auto& t : linksOf(p)) {
                if (!seen.insert(t.n).second) continue;
                if (isLatentNode(*t.n)) return true;
                for (auto* o : execOuts(*t.n)) todo.push_back(o);
                if (t.n->cls == "K2Node_Knot")
                    for (auto& kp : t.n->pins)
                        if (kp.dir == PinDir::Out) todo.push_back(&kp);
            }
        }
        return false;
    }
    int incomingExec(const PinRef& t) const { return static_cast<int>(linksOf(t.p).size()); }

    void emitExecOut(const Pin* out, Ctx& c, Out& o) {
        if (!out) return;
        auto t = linksOf(out);
        if (t.empty()) return;
        if (t.size() > 1) warn("an exec output is linked to several inputs; only the first link is followed");
        emitChain(t[0], c, o);
    }
    void emitBlock(const Pin* out, Ctx& c, Out& o) {
        o.ind++;
        size_t mark = c.path.size();
        c.depth++;
        if (c.depth > opt.limits.maxControlDepth) {
            o.line("// TODO(blueprint): flow nested too deeply - the rest was not converted");
            warn("control flow nested deeper than " + std::to_string(opt.limits.maxControlDepth) + " levels was cut");
        } else {
            emitExecOut(out, c, o);
        }
        c.depth--;
        c.path.resize(mark);
        o.ind--;
    }
    // A latent branch that must not block the flow after it (Sequence outputs, loop bodies): its own coroutine.
    void emitSubflow(const Pin* out, Ctx& c, Out& o, const std::string& what) {
        std::string fn = claim("Flow_" + std::to_string(++subflowCounter), "Flow");
        Out sub;
        sub.ind = 2;
        Ctx sc;
        sc.latent = true;
        sc.noHelpers = true;
        sc.path = c.path;
        sc.depth = c.depth;
        sc.loops = c.loops;
        emitExecOut(out, sc, sub);
        std::string f = "    // " + commentText(what, 100) + " (runs on its own like in Unreal: the flow after it does not wait)\n";
        f += "    Task " + fn + "() {\n" + sub.s + "        co_return;\n    }\n";
        privateFns.push_back(f);
        o.line("StartCoroutine(" + fn + "());");
    }

    void emitChain(PinRef t, Ctx& c, Out& o) {
        size_t base = c.path.size();
        int guard = 0;
        while (t.n && guard++ < 100000) {
            if (!fatal.empty()) break;
            if (++statements > opt.limits.maxEmittedStatements) {
                if (fatal.empty()) fatal = "the graph is too large or too complex after expanding merged flows (limit " + std::to_string(opt.limits.maxEmittedStatements) + " statements)";
                break;
            }
            const Node& n = *t.n;
            if (n.cls == "K2Node_Knot") {
                mark(n, Severity::Ignored, "reroute node");
                nodeState[&n].touched = true;
                const Pin* outp = nullptr;
                for (auto& p : n.pins)
                    if (p.dir == PinDir::Out) outp = &p;
                auto nx = linksOf(outp);
                t = nx.empty() ? PinRef{} : nx[0];
                continue;
            }
            // loop Break pins
            if (n.cls == "K2Node_MacroInstance" && t.p->name == "Break") {
                std::string brk = member(n.name + "#broken", "bool", shortTitle(n) + "_broken", "false");
                bool inside = std::find(c.loops.begin(), c.loops.end(), &n) != c.loops.end();
                if (inside) {
                    o.line(brk + " = true;");
                    if (!c.loops.empty() && c.loops.back() == &n) o.line("break;");
                } else {
                    o.line("// TODO(blueprint): Break of " + commentText(title(n), 60) + " from outside its loop body is not supported");
                    mark(n, Severity::Approximated, "Break from outside the loop body");
                }
                break;
            }
            if (std::find(c.path.begin(), c.path.end(), &n) != c.path.end()) {
                o.line("// TODO(blueprint): the flow loops back to " + commentText(title(n), 60) + " - exec cycles are not converted (use a loop node)");
                mark(n, Severity::Stubbed, "exec cycle");
                warn("an exec cycle back to " + commentText(title(n), 60) + " was cut");
                break;
            }
            if (!c.noHelpers && incomingExec(t) > 1) {  // merge point: one helper function shared by every incoming flow
                o.line(helper(t, c) + "();");
                break;
            }
            c.path.push_back(&n);
            if (!n.comment.empty()) o.line("// " + commentText(n.comment, 120));
            t = emitNode(t, c, o);
        }
        c.path.resize(base);
    }
    std::string helper(const PinRef& t, Ctx& c) {
        std::string key = t.n->name + "#" + t.p->id;
        if (auto it = helperByKey.find(key); it != helperByKey.end()) return it->second;
        std::string fn = claim("Flow_" + shortTitle(*t.n), "Flow");
        helperByKey[key] = fn;
        Out sub;
        Ctx hc;
        hc.path = c.path;
        hc.depth = c.depth;
        hc.loops = c.loops;
        hc.noHelpers = false;
        // the helper's own entry is allowed once more (it is the merge point)
        Ctx copy = hc;
        PinRef start = t;
        size_t base = copy.path.size();
        copy.path.push_back(start.n);
        if (!start.n->comment.empty()) sub.line("// " + commentText(start.n->comment, 120));
        PinRef next = emitNode(start, copy, sub);
        if (next.n) emitChain(next, copy, sub);
        copy.path.resize(base);
        helperFns.push_back("    // Shared by several flows (Unreal: more than one wire into " + commentText(title(*t.n), 60) + ")\n    void " + fn + "() {\n" + sub.s + "    }\n");
        return fn;
    }

    std::string sevOf(const FunctionMapping& fm) const { return fm.status; }
    Severity sevFrom(const std::string& s) const {
        return s == "approximated" ? Severity::Approximated : s == "stubbed" ? Severity::Stubbed : Severity::Converted;
    }

    // Emits one exec node; returns where the flow continues (or {} when the node ends it / handled its outputs).
    PinRef emitNode(const PinRef& t, Ctx& c, Out& o) {
        const Node& n = *t.n;
        const std::string& k = n.cls;
        auto next = [&](const Pin* p) -> PinRef {
            auto l = linksOf(p);
            if (l.size() > 1) warn("an exec output is linked to several inputs; only the first link is followed");
            return l.empty() ? PinRef{} : l[0];
        };
        if (k == "K2Node_CallFunction" || k == "K2Node_CallArrayFunction" || k == "K2Node_CallParentFunction") return emitCall(n, c, o, next);
        if (k == "K2Node_VariableSet") {
            const Pin* self = dataIn(n, "self");
            const Pin* vp = nullptr;
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && isDataPin(p) && p.name != "self") vp = &p;
            if (self && !linksOf(self).empty() && !n.selfContext) {
                o.line("// TODO(blueprint): setting '" + commentText(n.memberName, 60) + "' on another object is not converted");
                mark(n, Severity::Stubbed, "variable of another object");
                unrecognised("variable on another object", n.memberName, &n, "setting a variable of another Blueprint");
                return next(execOut(n, "then"));
            }
            Var& v = variable(n.memberName, vp ? &vp->type : nullptr);
            o.line(v.cpp + " = " + (vp ? coerce(inputExpr(n, vp, c, 0), v.ty) : defaultOf(v.ty)) + ";");
            mark(n, Severity::Converted);
            return next(execOut(n, "then"));
        }
        if (k == "K2Node_IfThenElse") {
            mark(n, Severity::Converted);
            std::string cond = arg(n, "Condition", Ty{K::Bool}, c, 0);
            const Pin *th = execOut(n, "then"), *el = execOut(n, "else");
            bool hasThen = th && !th->links.empty(), hasElse = el && !el->links.empty();
            if (!hasThen && !hasElse) {
                o.line("(void)(" + cond + ");");
                return {};
            }
            o.line(hasThen ? "if (" + cond + ") {" : "if (!(" + cond + ")) {");
            emitBlock(hasThen ? th : el, c, o);
            if (hasThen && hasElse) {
                o.line("} else {");
                emitBlock(el, c, o);
            }
            o.line("}");
            return {};
        }
        if (k == "K2Node_ExecutionSequence") {
            mark(n, Severity::Converted);
            auto outs = execOuts(n);
            std::vector<const Pin*> linked;
            for (auto* p : outs)
                if (!p->links.empty()) linked.push_back(p);
            for (size_t i = 0; i < linked.size(); ++i) {
                bool last = i + 1 == linked.size();
                if (c.latent && !last && reachesLatent(linked[i])) {
                    emitSubflow(linked[i], c, o, "Sequence output " + linked[i]->name);
                    continue;
                }
                if (last) return next(linked[i]);
                o.line("{  // " + commentText(linked[i]->name, 40));
                emitBlock(linked[i], c, o);
                o.line("}");
            }
            return {};
        }
        if (k == "K2Node_MacroInstance") return emitMacro(t, c, o, next);
        if (k == "K2Node_SwitchInteger" || k == "K2Node_SwitchEnum") {
            bool isEnum = k == "K2Node_SwitchEnum";
            mark(n, isEnum ? Severity::Approximated : Severity::Converted, isEnum ? "enum values are their index" : "");
            std::string sel = arg(n, "Selection", Ty{K::Int}, c, 0);
            o.line("switch (" + sel + ") {");
            int idx = 0;
            const Pin* def = nullptr;
            long long startIdx = static_cast<long long>(parseNum(n.props.contains("StartIndex") ? n.props.at("StartIndex") : "0"));
            for (auto* p : execOuts(n)) {
                if (p->name == "Default") {
                    def = p;
                    continue;
                }
                long long v = isEnum ? idx : startIdx + idx;
                if (!isEnum) {
                    bool ok = false;
                    double d = parseNum(p->name, &ok);
                    if (ok) v = static_cast<long long>(d);
                }
                ++idx;
                if (p->links.empty()) continue;
                o.line("case " + intLit(v) + ": {");
                emitBlock(p, c, o);
                o.ind++;
                o.line("break;");
                o.ind--;
                o.line("}");
            }
            if (def && !def->links.empty()) {
                o.line("default: {");
                emitBlock(def, c, o);
                o.ind++;
                o.line("break;");
                o.ind--;
                o.line("}");
            } else {
                o.line("default: break;");
            }
            o.line("}");
            return {};
        }
        if (k == "K2Node_SwitchString" || k == "K2Node_SwitchName") {
            mark(n, Severity::Converted);
            bool cs = toLower(n.props.contains("bIsCaseSensitive") ? unquote(n.props.at("bIsCaseSensitive")) : "false") == "true";
            std::string sel = arg(n, "Selection", Ty{K::String}, c, 0);
            std::string local = "bp_sel_" + std::to_string(statements);
            o.line("{");
            o.ind++;
            o.line("const std::string " + local + " = " + (cs ? sel : "bp::ToLower(" + sel + ")") + ";");
            bool first = true;
            const Pin* def = nullptr;
            for (auto* p : execOuts(n)) {
                if (p->name == "Default") {
                    def = p;
                    continue;
                }
                std::string lit = cppStringLiteral(cs ? p->name : toLower(p->name));
                o.line(std::string(first ? "if (" : "} else if (") + local + " == " + lit + ") {");
                first = false;
                emitBlock(p, c, o);
            }
            if (def && !def->links.empty()) {
                o.line(first ? "{" : "} else {");
                emitBlock(def, c, o);
                o.line("}");
            } else if (!first) {
                o.line("}");
            }
            o.ind--;
            o.line("}");
            return {};
        }
        if (k == "K2Node_DynamicCast") {
            mark(n, Severity::Approximated, "Fledge has no actor classes: any valid entity passes the cast");
            std::string m = member(n.name + "#as", "Entity", "As" + n.memberName);
            o.line(m + " = " + arg(n, "Object", Ty{K::Entity}, c, 0) + ";  // Cast To " + commentText(n.memberName, 60) + " (approximated: no class check)");
            const Pin *ok = execOut(n, "then"), *fail = execOut(n, "CastFailed");
            bool hasFail = fail && !fail->links.empty();
            if (!hasFail) {
                o.line("if (" + m + ".IsValid()) {");
                emitBlock(ok, c, o);
                o.line("}");
                return {};
            }
            o.line("if (" + m + ".IsValid()) {");
            emitBlock(ok, c, o);
            o.line("} else {");
            emitBlock(fail, c, o);
            o.line("}");
            return {};
        }
        if (k == "K2Node_CallDelegate") {
            auto it = dispatchers.find(n.memberName);
            if (it == dispatchers.end()) {
                o.line("// TODO(blueprint): event dispatcher '" + commentText(n.memberName, 60) + "' is unknown");
                mark(n, Severity::Stubbed, "unknown dispatcher");
                return next(execOut(n, "then"));
            }
            std::string a = selfOf(n, c, 0).code;
            for (auto& [fname, fty] : it->second.fields) a += ", " + arg(n, fname, fty, c, 0);
            o.line("Events::Emit(" + it->second.structName + "{" + a + "});  // Unreal: Call " + commentText(n.memberName, 60));
            mark(n, Severity::Approximated, "event dispatchers use the Fledge event bus (Events::Emit / Subscribe)");
            return next(execOut(n, "then"));
        }
        if (k == "K2Node_AddDelegate" || k == "K2Node_AssignDelegate" || k == "K2Node_RemoveDelegate" || k == "K2Node_ClearDelegate") {
            auto it = dispatchers.find(n.memberName);
            if (it == dispatchers.end()) {
                o.line("// TODO(blueprint): event dispatcher '" + commentText(n.memberName, 60) + "' is unknown");
                mark(n, Severity::Stubbed, "unknown dispatcher");
                return next(execOut(n, "then"));
            }
            const Dispatcher& d = it->second;
            if (k == "K2Node_ClearDelegate") {
                for (auto& s : subsByDispatcher[d.ue]) o.line("Events::Unsubscribe(" + s + "); " + s + " = 0;");
                mark(n, Severity::Converted);
                return next(execOut(n, "then"));
            }
            const Root* h = delegateTarget(n, "Delegate");
            if (k == "K2Node_AssignDelegate" && !h) {  // Assign creates its own custom event, linked by its OutputDelegate / then
                for (auto& p : n.pins)
                    if (p.type.category == "delegate")
                        for (auto& l : linksOf(&p))
                            for (auto& r : roots)
                                if (r.node == l.n) h = &r;
            }
            if (!h) {
                o.line("// TODO(blueprint): the event bound to '" + commentText(n.memberName, 60) + "' was not found in the input");
                mark(n, Severity::Stubbed, "bound event not found");
                return next(execOut(n, "then"));
            }
            std::string sub = member("sub#" + d.ue + "#" + h->fn, "SubscriptionId", "Sub_" + d.ue + "_" + h->fn, "0");
            auto& subs = subsByDispatcher[d.ue];
            if (std::find(subs.begin(), subs.end(), sub) == subs.end()) subs.push_back(sub);
            if (k == "K2Node_RemoveDelegate") {
                o.line("Events::Unsubscribe(" + sub + "); " + sub + " = 0;");
                mark(n, Severity::Converted);
                return next(execOut(n, "then"));
            }
            std::string src = selfOf(n, c, 0).code;
            std::string call = h->fn + "(";
            for (size_t i = 0; i < h->params.size(); ++i) {
                if (i) call += ", ";
                call += i < d.fields.size() ? "bp::As<" + cppOf(h->params[i].ty) + ">(e." + fieldName(d, i) + ")" : defaultOf(h->params[i].ty);
            }
            call += ")";
            o.line("if (" + sub + ") Events::Unsubscribe(" + sub + ");");
            o.line(sub + " = Subscribe<" + d.structName + ">([this, bp_src = " + src + "](const " + d.structName + "& e) {");
            o.line("    if (e.sender == bp_src) " + call + ";");
            o.line("});");
            mark(n, Severity::Approximated, "bound through the Fledge event bus");
            return next(execOut(n, "then"));
        }
        if (k == "K2Node_FunctionResult") {
            if (!c.fnRoot) {
                o.line("// TODO(blueprint): a Return node outside a function");
                mark(n, Severity::Stubbed, "return outside a function");
                return {};
            }
            mark(n, Severity::Converted);
            const Root& r = *c.fnRoot;
            if (r.outs.size() == 1) {
                o.line("return " + arg(n, r.outs[0].ue, r.outs[0].ty, c, 0) + ";");
            } else {
                for (auto& out : r.outs) o.line(out.name + " = " + arg(n, out.ue, out.ty, c, 0) + ";");
                o.line("return;");
            }
            return {};
        }
        if (k == "K2Node_SpawnActorFromClass") {
            const Pin* rv = n.pin("ReturnValue", PinDir::Out);
            std::string clsArg = arg(n, "Class", Ty{K::Class}, c, 0), xf = arg(n, "SpawnTransform", Ty{K::Transform}, c, 0);
            o.line("{");
            o.ind++;
            o.line("const bp::Transform bp_xf = " + xf + ";");
            std::string spawn = "Scene::Spawn(bp::PrefabFor(" + clsArg + ").c_str(), bp::PosFromUE(bp_xf.location), bp::RotatorToQuat(bp_xf.rotation))";
            o.line(rv ? assign(outMember(n, *rv), spawn) : spawn + ";");
            o.ind--;
            o.line("}");
            mark(n, Severity::Approximated, "spawns the prefab Prefabs/<Class>.fscene; exposed-on-spawn pins are ignored");
            return next(execOut(n, "then"));
        }
        if (k == "K2Node_Timeline") {
            o.line("// TODO(blueprint): Timeline '" + commentText(n.memberName, 60) + "' is not converted - animate it with a coroutine (WaitFrames / Lerp) "
                   "or an Animator. Its Finished output runs right away; Update never runs.");
            mark(n, Severity::Stubbed, "timelines are not converted (Finished runs immediately, Update never)");
            unrecognised("node class", "K2Node_Timeline", &n, "timeline");
            return next(execOut(n, "Finished"));
        }
        // unknown impure node: stub, then continue through its single exec output
        o.line("// TODO(blueprint): " + commentText(title(n), 80) + " - no Fledge equivalent");
        mark(n, Severity::Stubbed, "node class not supported");
        unrecognised("node class", k, &n, "impure node");
        return next(firstExecOut(n));
    }

    const Root* delegateTarget(const Node& n, std::string_view pinName) {
        const Pin* dp = nullptr;
        for (auto& p : n.pins)
            if (p.dir == PinDir::In && p.name == pinName) dp = &p;
        for (auto& l : linksOf(dp)) {
            if (l.n->cls == "K2Node_CreateDelegate") {
                mark(*l.n, Severity::Converted);
                auto it = rootByName.find(lowerStr(l.n->memberName));
                if (it != rootByName.end()) return &roots[it->second];
            }
            for (auto& r : roots)
                if (r.node == l.n) return &r;
        }
        return nullptr;
    }

    template <typename Next> PinRef emitCall(const Node& n, Ctx& c, Out& o, Next next) {
        const Pin* then = execOut(n, "then");
        if (n.cls == "K2Node_CallParentFunction") {
            o.line("// (Unreal 'Parent: " + commentText(n.memberName, 60) + "' call - a Fledge behaviour has no parent Blueprint)");
            mark(n, Severity::Ignored, "parent call: nothing to call in Fledge");
            nodeState[&n].touched = true;
            return next(then);
        }
        if (const Root* r = selfTarget(n)) {
            std::string a = callArgs(n, *r, c, 0);
            if (r->kind == "function" && !r->outs.empty()) {
                if (r->outs.size() == 1) {
                    const Pin* cp = n.pin(r->outs[0].ue, PinDir::Out);
                    if (cp && !cp->links.empty()) o.line(assign(outMember(n, *cp, r->outs[0].ty), r->fn + "(" + a + ")"));
                    else o.line(r->fn + "(" + a + ");");
                } else {
                    std::string extra;
                    for (auto& out : r->outs) {
                        const Pin* cp = n.pin(out.ue, PinDir::Out);
                        extra += (a.empty() && extra.empty() ? "" : ", ") +
                                 (cp ? outMember(n, *cp, out.ty) : member(n.name + "#" + out.ue, cppOf(out.ty), shortTitle(n) + "_" + out.ue, "", "", out.ty));
                    }
                    o.line(r->fn + "(" + a + extra + ");");
                }
            } else {
                o.line(r->fn + "(" + a + ");");
            }
            mark(n, Severity::Converted);
            return next(then);
        }
        const FunctionMapping* fm = map.function(n.memberName, n.memberParent);
        if (!fm) {
            if (n.selfContext) {
                std::string a;
                std::vector<const Pin*> outs;
                Ty ret;
                std::string fn = stubFunction(n, c, 0, a, outs, ret);
                if (outs.size() == 1 && !outs[0]->links.empty()) o.line(outMember(n, *outs[0]) + " = " + fn + "(" + a + ");");
                else if (outs.size() > 1) {
                    std::string extra;
                    for (auto* p : outs) extra += (a.empty() && extra.empty() ? "" : ", ") + outMember(n, *p);
                    o.line(fn + "(" + a + extra + ");");
                } else o.line(fn + "(" + a + ");");
                mark(n, Severity::Stubbed, "function not in the input (stub member generated)");
                return next(then);
            }
            std::string who = (n.memberParent.empty() ? "" : n.memberParent + "::") + n.memberName;
            o.line("// TODO(blueprint): " + commentText(who, 100) + " - no Fledge equivalent");
            mark(n, Severity::Stubbed, "no Fledge equivalent");
            unrecognised("function", who, &n, n.pin("LatentInfo") ? "latent function (continues immediately)" : "no mapping");
            return next(then);
        }
        Severity sev = sevFrom(fm->status);
        if (!fm->handler.empty()) return emitHandler(n, *fm, c, o, next);
        if (fm->stmt.empty()) {  // a pure mapping used on an impure node: evaluate its outputs into members
            bool any = false;
            for (auto& p : n.pins)
                if (p.dir == PinDir::Out && isDataPin(p) && !p.links.empty()) {
                    bool failed = false;
                    std::string tpl = p.name == "ReturnValue" && !fm->expr.empty() ? fm->expr : (fm->outputs.contains(p.name) ? fm->outputs.at(p.name) : "");
                    if (!fm->op.empty() && p.name == "ReturnValue") {
                        Ty want = fm->args.contains("A") ? tyFromName(fm->args.at("A"), tyOf(p.type)) : tyOf(p.type);
                        std::string acc;
                        for (auto& ip : n.pins)
                            if (ip.dir == PinDir::In && isDataPin(ip) && ip.name != "self") {
                                std::string a = coerce(inputExpr(n, &ip, c, 0), want);
                                acc = acc.empty() ? a : acc + " " + fm->op + " " + a;
                            }
                        o.line(assign(outMember(n, p), "(" + (acc.empty() ? defaultOf(want) : acc) + ")"));
                        any = true;
                        continue;
                    }
                    if (tpl.empty()) continue;
                    std::string code = expand(tpl, n, *fm, c, 0, failed);
                    if (failed) {
                        sev = Severity::Stubbed;
                        continue;
                    }
                    o.line(assign(outMember(n, p), code));
                    any = true;
                }
            if (!any) o.line("// " + commentText(stripK2(n.memberName), 80) + " (result unused)");
            mark(n, sev, fm->note);
            return next(then);
        }
        bool failed = false;
        std::string code = expand(fm->stmt, n, *fm, c, 0, failed);
        if (failed) {
            o.line("// TODO(blueprint): " + commentText(stripK2(n.memberName), 80) + " - the pin types do not match the mapping (a container was expected)");
            mark(n, Severity::Stubbed, "argument types do not match the mapping");
            return next(then);
        }
        if (startsWith(fm->stmt, "${member:") && code.size() > 2 && code.back() == ';') {  // "m = rhs;" -> typed assignment
            size_t eq = code.find(" = ");
            std::string pin = fm->stmt.substr(9, fm->stmt.find('}') - 9);
            const Pin* op = n.pin(pin, PinDir::Out);
            if (eq != std::string::npos && fm->stmt.find(';') == fm->stmt.size() - 1) {
                std::string rhs = code.substr(eq + 3, code.size() - eq - 4);
                code = op && op->links.empty() ? "(void)" + rhs + ";" : assign(code.substr(0, eq), rhs);
            }
        }
        o.line(code + (sev == Severity::Converted ? std::string() : "  // " + std::string(sev == Severity::Stubbed ? "TODO(blueprint): " : "approximated: ") + commentText(fm->note.empty() ? stripK2(n.memberName) : fm->note, 120)));
        for (auto& [pn, tpl] : fm->outputs) {
            const Pin* p = n.pin(pn, PinDir::Out);
            if (!p || p->links.empty()) continue;
            bool f2 = false;
            std::string v = expand(tpl, n, *fm, c, 0, f2);
            if (!f2) o.line(assign(outMember(n, *p), v));
        }
        for (auto& p : n.pins)
            if (p.dir == PinDir::Out && isDataPin(p) && !p.links.empty() && !fm->outputs.contains(p.name) && fm->stmt.find("${member:" + p.name + "}") == std::string::npos) {
                warn("output '" + commentText(p.name, 40) + "' of " + commentText(stripK2(n.memberName), 60) + " is not mapped (default value)");
                if (sev == Severity::Converted) sev = Severity::Approximated;
            }
        mark(n, sev, fm->note);
        return next(then);
    }

    template <typename Next> PinRef emitHandler(const Node& n, const FunctionMapping& fm, Ctx& c, Out& o, Next next) {
        const Pin* then = execOut(n, "then");
        const std::string& h = fm.handler;
        if (h == "delay" || h == "retriggerable_delay" || h == "next_tick") {
            if (!c.latent) {
                o.line("// TODO(blueprint): " + commentText(stripK2(n.memberName), 40) + " is not allowed here (functions cannot wait) - continuing immediately");
                mark(n, Severity::Stubbed, "latent node outside an event");
                return next(then);
            }
            std::string base = shortTitle(n) + "_" + std::to_string(statements);
            if (h == "next_tick") {
                o.line("co_await NextFrame();");
            } else if (h == "delay") {
                std::string flag = member(n.name + "#pending", "bool", shortTitle(n) + "_pending", "false", "Unreal ignores a Delay that is already counting down");
                o.line("if (" + flag + ") co_return;  // Delay already counting down");
                o.line(flag + " = true;");
                o.line("co_await WaitSeconds(" + arg(n, "Duration", Ty{K::Float}, c, 0) + ");");
                o.line(flag + " = false;");
            } else {
                std::string gen = member(n.name + "#gen", "int", shortTitle(n) + "_generation", "0", "RetriggerableDelay restarts on every trigger");
                std::string local = "bp_gen_" + std::to_string(statements);
                o.line("const int " + local + " = ++" + gen + ";");
                o.line("co_await WaitSeconds(" + arg(n, "Duration", Ty{K::Float}, c, 0) + ");");
                o.line("if (" + local + " != " + gen + ") co_return;  // retriggered meanwhile");
            }
            mark(n, Severity::Converted);
            return next(then);
        }
        if (h == "timer_by_name" || h == "timer_by_event") {
            const Root* target = nullptr;
            if (h == "timer_by_name") {
                const Pin* fnp = dataIn(n, "FunctionName");
                if (fnp && linksOf(fnp).empty()) {
                    auto it = rootByName.find(lowerStr(fnp->defaultValue));
                    if (it != rootByName.end()) target = &roots[it->second];
                }
            } else {
                target = delegateTarget(n, "Delegate");
            }
            if (!target || !target->params.empty()) {
                o.line("// TODO(blueprint): timer target not found (or it takes parameters) - " + commentText(stripK2(n.memberName), 40));
                mark(n, Severity::Stubbed, "timer target not found");
                return next(then);
            }
            std::string tm = member("timer#" + target->fn, "TimerHandle", "Timer_" + target->fn, "0", "timer that calls " + target->fn);
            std::string t = arg(n, "Time", Ty{K::Float}, c, 0), loop = arg(n, "bLooping", Ty{K::Bool}, c, 0);
            o.line("CancelTimer(" + tm + ");  // Unreal replaces an existing timer for the same function");
            o.line(tm + " = (" + loop + ") ? Every(" + t + ", [this] { " + target->fn + "(); }) : After(" + t + ", [this] { " + target->fn + "(); });");
            if (const Pin* rv = n.pin("ReturnValue", PinDir::Out); rv && !rv->links.empty()) o.line(assign(outMember(n, *rv), tm));
            mark(n, sevFrom(fm.status), fm.note);
            return next(then);
        }
        if (h == "clear_timer_by_name") {
            const Pin* fnp = dataIn(n, "FunctionName");
            auto it = fnp ? rootByName.find(lowerStr(fnp->defaultValue)) : rootByName.end();
            if (it == rootByName.end()) {
                o.line("// TODO(blueprint): ClearTimer target not found");
                mark(n, Severity::Stubbed, "timer target not found");
                return next(then);
            }
            std::string tm = member("timer#" + roots[it->second].fn, "TimerHandle", "Timer_" + roots[it->second].fn, "0");
            o.line("CancelTimer(" + tm + "); " + tm + " = 0;");
            mark(n, Severity::Converted);
            return next(then);
        }
        if (h == "movement") {
            usesMovement = true;
            std::string acc = member("movement#input", "Vec3", "MoveInput", "", "AddMovementInput, applied at the end of OnUpdate");
            o.line(acc + " += bp::DirFromUE(" + arg(n, "WorldDirection", Ty{K::Vector}, c, 0) + ") * (" + arg(n, "ScaleValue", Ty{K::Float}, c, 0) + ");  // approximated: AddMovementInput");
            mark(n, sevFrom(fm.status), fm.note);
            return next(then);
        }
        o.line("// TODO(blueprint): handler '" + commentText(h, 40) + "' is not implemented");
        mark(n, Severity::Stubbed, "handler missing");
        return next(then);
    }

    template <typename Next> PinRef emitMacro(const PinRef& t, Ctx& c, Out& o, Next next) {
        const Node& n = *t.n;
        std::string m = lowerStr(n.memberName);
        std::string in = t.p->name;
        auto out = [&](const char* name) { return execOut(n, name); };
        bool standard = lowerStr(n.memberParent).find("standardmacros") != std::string::npos || n.memberParent.empty();
        if (!standard) m = "?";
        auto loopBody = [&](const Pin* body, const std::string& what) {
            c.loops.push_back(&n);
            if (c.latent && reachesLatent(body)) {
                o.ind++;
                emitSubflow(body, c, o, what);
                o.ind--;
            } else {
                emitBlock(body, c, o);
            }
            c.loops.pop_back();
        };
        if (m == "forloop" || m == "forloopwithbreak") {
            mark(n, Severity::Converted);
            std::string idx = member(n.name + "#Index", "int", shortTitle(n) + "_Index", "0");
            std::string cond;
            if (m == "forloopwithbreak") {
                std::string brk = member(n.name + "#broken", "bool", shortTitle(n) + "_broken", "false");
                o.line(brk + " = false;");
                cond = " && !" + brk;
            }
            o.line("for (" + idx + " = " + arg(n, "FirstIndex", Ty{K::Int}, c, 0) + "; " + idx + " <= " + arg(n, "LastIndex", Ty{K::Int}, c, 0) + cond + "; ++" + idx + ") {");
            loopBody(out("LoopBody"), "ForLoop body");
            o.line("}");
            return next(out("Completed"));
        }
        if (m == "foreachloop" || m == "foreachloopwithbreak" || m == "reverseforeachloop") {
            const Pin* arrPin = dataIn(n, "Array");
            Expr a = inputExpr(n, arrPin, c, 0);
            if (a.ty.cont != 'A' && a.ty.cont != 'S') {
                o.line("// TODO(blueprint): " + commentText(n.memberName, 40) + " over something that is not an array");
                mark(n, Severity::Stubbed, "array input missing");
                return next(out("Completed"));
            }
            mark(n, Severity::Converted);
            Ty el = a.ty;
            el.cont = 0;
            const Pin* elemPin = n.pin("Array Element", PinDir::Out);
            std::string idx = member(n.name + "#Array Index", "int", shortTitle(n) + "_Index", "0");
            std::string elem = elemPin ? member(n.name + "#Array Element", cppOf(el), shortTitle(n) + "_Element") : member(n.name + "#el", cppOf(el), shortTitle(n) + "_Element");
            std::string cond;
            if (m == "foreachloopwithbreak") {
                std::string brk = member(n.name + "#broken", "bool", shortTitle(n) + "_broken", "false");
                o.line(brk + " = false;");
                cond = " && !" + brk;
            }
            if (m == "reverseforeachloop")
                o.line("for (" + idx + " = static_cast<int>((" + a.code + ").size()) - 1; " + idx + " >= 0" + cond + "; --" + idx + ") {");
            else
                o.line("for (" + idx + " = 0; " + idx + " < static_cast<int>((" + a.code + ").size())" + cond + "; ++" + idx + ") {");
            o.ind++;
            o.line("if (" + idx + " >= static_cast<int>((" + a.code + ").size())) break;");
            o.line(assign(elem, "(" + a.code + ")[static_cast<size_t>(" + idx + ")]"));
            o.ind--;
            loopBody(out("LoopBody"), "ForEachLoop body");
            o.line("}");
            return next(out("Completed"));
        }
        if (m == "whileloop") {
            mark(n, Severity::Converted);
            std::string guard = member(n.name + "#guard", "int", shortTitle(n) + "_iterations", "0");
            o.line("for (" + guard + " = 0; " + arg(n, "Condition", Ty{K::Bool}, c, 0) + "; ++" + guard + ") {");
            o.ind++;
            o.line("if (" + guard + " >= bp::kMaxLoopIterations) { Log::Error(\"{}\", std::string(\"Blueprint WhileLoop: runaway loop stopped\")); break; }");
            o.ind--;
            loopBody(out("LoopBody"), "WhileLoop body");
            o.line("}");
            return next(out("Completed"));
        }
        if (m == "doonce") {
            mark(n, Severity::Converted);
            const Pin* sc = dataIn(n, "Start Closed");
            std::string init = sc && linksOf(sc).empty() && toLower(sc->defaultValue) == "true" ? "{true}" : "";
            std::string st = member(n.name + "#state", "bp::DoOnce", "DoOnce", init);
            if (in == "Reset") {
                o.line(st + ".Reset();");
                return {};
            }
            o.line("if (" + st + ".Enter()) {");
            emitBlock(out("Completed"), c, o);
            o.line("}");
            return {};
        }
        if (m == "don") {
            mark(n, Severity::Converted);
            std::string st = member(n.name + "#state", "bp::DoN", "DoN");
            if (in == "Reset") {
                o.line(st + ".Reset();");
                return {};
            }
            o.line("if (" + st + ".Enter(" + arg(n, "N", Ty{K::Int}, c, 0) + ")) {");
            emitBlock(out("Exit"), c, o);
            o.line("}");
            return {};
        }
        if (m == "gate") {
            mark(n, Severity::Converted);
            const Pin* sc = dataIn(n, "Start Closed");
            std::string init = sc && linksOf(sc).empty() && toLower(sc->defaultValue) == "true" ? "{false}" : "";
            std::string st = member(n.name + "#state", "bp::Gate", "Gate", init);
            if (in == "Open") o.line(st + ".open = true;");
            else if (in == "Close") o.line(st + ".open = false;");
            else if (in == "Toggle") o.line(st + ".open = !" + st + ".open;");
            else {
                o.line("if (" + st + ".open) {");
                emitBlock(out("Exit"), c, o);
                o.line("}");
            }
            return {};
        }
        if (m == "flipflop") {
            mark(n, Severity::Converted);
            std::string st = member(n.name + "#state", "bp::FlipFlop", "FlipFlop");
            const Pin *a = out("A"), *b = out("B");
            o.line("if (" + st + ".Flip()) {");
            emitBlock(a, c, o);
            o.line("} else {");
            emitBlock(b, c, o);
            o.line("}");
            return {};
        }
        if (m == "isvalid") {
            mark(n, Severity::Converted);
            const Pin* obj = dataIn(n, "InputObject");
            std::string v = obj ? "bp::IsValid(" + inputExpr(n, obj, c, 0).code + ")" : std::string("false");
            const Pin *yes = out("Is Valid"), *no = out("Is Not Valid");
            o.line("if (" + v + ") {");
            emitBlock(yes, c, o);
            if (no && !no->links.empty()) {
                o.line("} else {");
                emitBlock(no, c, o);
            }
            o.line("}");
            return {};
        }
        o.line("// TODO(blueprint): macro '" + commentText(n.memberName, 60) + "' is not supported");
        mark(n, Severity::Stubbed, standard ? "macro not supported" : "user macros are not converted (collapse them into a function)");
        unrecognised("macro", n.memberName, &n, standard ? "standard macro without a converter" : "macro from " + n.memberParent);
        return {};
    }

    // ----------------------------------------------------------------------------------------- roots --
    std::vector<Param> paramsOf(const Node& n, PinDir dir) {
        std::vector<Param> ps;
        for (auto& p : n.pins) {
            if (p.dir != dir || !isDataPin(p) || p.name == "self") continue;
            Ty t = tyOf(p.type);
            if (t.k == K::Void) continue;
            ps.push_back({p.name, claim(p.name, "Arg"), t, &p});
        }
        return ps;
    }
    void addRoot(Root r, const std::string& name = {}) {
        std::string key = lowerStr(r.kind == "custom" || r.kind == "function" ? (name.empty() ? r.node->memberName : name) : std::string());
        roots.push_back(std::move(r));
        if (!key.empty() && !rootByName.contains(key)) rootByName[key] = roots.size() - 1;
    }
    void collectRoots() {
        std::set<std::string> seenFunctions;
        for (auto& n : g.nodes) {
            const std::string& k = n.cls;
            if (k == "K2Node_Event" || k == "K2Node_ComponentBoundEvent" || k == "K2Node_ActorBoundEvent") {
                Root r;
                r.node = &n;
                r.start = execOut(n, "then");
                r.kind = "event";
                r.em = map.event(n.memberName);
                std::string t = r.em ? r.em->title : stripK2(n.memberName);
                if (startsWith(t, "Receive")) t = t.substr(7);
                if (k != "K2Node_Event" && !n.memberParent.empty()) t += "_" + n.memberParent;
                r.title = t;
                r.fn = claim("Event_" + t, "Event");
                names.push_back({n.memberName, r.fn, "event"});
                r.callback = r.em ? r.em->callback : "";
                r.isPublic = r.callback.empty();
                if (!r.em) {
                    r.sev = Severity::Stubbed;
                    r.sevNote = "event with no Fledge equivalent: the function is generated but nothing calls it";
                    unrecognised("event", n.memberName, &n, "no mapping; generated as an uncalled member function");
                } else {
                    r.sev = sevFrom(r.em->status);
                    r.sevNote = r.em->note;
                }
                r.params = paramsOf(n, PinDir::Out);
                addRoot(std::move(r));
            } else if (k == "K2Node_CustomEvent") {
                Root r;
                r.node = &n;
                r.start = execOut(n, "then");
                r.kind = "custom";
                r.title = n.memberName;
                r.fn = claim(n.memberName, "CustomEvent");
                names.push_back({n.memberName, r.fn, "custom event"});
                r.isPublic = true;
                r.params = paramsOf(n, PinDir::Out);
                addRoot(std::move(r));
            } else if (k == "K2Node_FunctionEntry") {
                std::string fname = n.memberName.empty() ? n.graph : n.memberName;
                if (fname.empty() || !seenFunctions.insert(lowerStr(fname)).second) continue;
                Root r;
                r.node = &n;
                r.start = execOut(n, "then");
                r.kind = "function";
                r.title = fname;
                r.fn = claim(fname, "Function");
                names.push_back({fname, r.fn, "function"});
                r.isPublic = true;
                r.params = paramsOf(n, PinDir::Out);
                for (auto& rn : g.nodes)
                    if (rn.cls == "K2Node_FunctionResult" && (lowerStr(rn.memberName) == lowerStr(fname) || (!n.graph.empty() && rn.graph == n.graph))) {
                        for (auto& p : rn.pins)
                            if (p.dir == PinDir::In && isDataPin(p) && tyOf(p.type).k != K::Void) r.outs.push_back({p.name, claim(p.name, "Out"), tyOf(p.type), &p});
                        break;
                    }
                addRoot(std::move(r), fname);
            } else if (k == "K2Node_InputAction" || k == "K2Node_InputKey" || k == "K2Node_EnhancedInputAction") {
                std::string action, label;
                Severity sev = Severity::Converted;
                std::string note;
                if (k == "K2Node_InputAction") {
                    action = map.inputAction(n.memberName);
                    label = "InputAction_" + n.memberName;
                    static const std::set<std::string> defaults = {"Jump", "Sprint", "Interact", "Fire", "Aim", "Pause", "Crouch", "Reload"};
                    if (!defaults.contains(action)) {
                        sev = Severity::Approximated;
                        note = "add the action '" + action + "' to the project's input map";
                    }
                } else if (k == "K2Node_InputKey") {
                    std::string key = map.key(n.memberName);
                    label = "InputKey_" + n.memberName;
                    action = "BP_Key_" + sanitizeIdentifier(n.memberName, "Key");
                    if (key.empty()) {
                        sev = Severity::Stubbed;
                        note = "unknown key '" + n.memberName + "'";
                        unrecognised("input key", n.memberName, &n, "no Fledge key name");
                    } else {
                        startPrologue.push_back("Input::Rebind(" + cppStringLiteral(action) + ", " + cppStringLiteral(key) + ");  // Unreal key " + commentText(n.memberName, 40));
                        sev = Severity::Approximated;
                        note = "a private input action bound to the key in OnStart";
                    }
                } else {
                    std::string ia = n.memberName;
                    if (startsWith(ia, "IA_")) ia = ia.substr(3);
                    action = map.inputAction(ia);
                    label = "Input_" + ia;
                    sev = Severity::Approximated;
                    note = "Enhanced Input: Started = pressed, Triggered / Ongoing = held, Completed = released, Canceled is never called";
                }
                for (auto* sp : execOuts(n)) {
                    if (sp->links.empty()) continue;
                    std::string when;
                    const std::string& pn = sp->name;
                    if (pn == "Pressed" || pn == "Started") when = "Input::Pressed";
                    else if (pn == "Released" || pn == "Completed") when = "Input::Released";
                    else if (pn == "Triggered" || pn == "Ongoing") when = "Input::Down";
                    else continue;
                    Root r;
                    r.node = &n;
                    r.start = sp;
                    r.kind = "input";
                    r.title = label + "_" + pn;
                    r.fn = claim(r.title, "Input");
                    names.push_back({title(n) + " " + pn, r.fn, "input event"});
                    r.params = paramsOf(n, PinDir::Out);
                    std::string args;
                    for (auto& pa : r.params) {
                        std::string v;
                        bool value = pa.ue == "ActionValue";
                        if (value && pa.ty.k == K::Float) v = "Input::Axis(" + cppStringLiteral(map.inputAxis(action)) + ")";
                        else if (value && pa.ty.k == K::Bool) v = "Input::Down(" + cppStringLiteral(action) + ")";
                        else v = defaultOf(pa.ty);  // Key, ElapsedSeconds, TriggeredSeconds, 2D values: not available
                        args += (args.empty() ? "" : ", ") + v;
                    }
                    r.dispatch = "if (" + when + "(" + cppStringLiteral(action) + ")) " + r.fn + "(" + args + ");";
                    r.sev = sev;
                    r.sevNote = note;
                    addRoot(std::move(r));
                }
            } else if (k == "K2Node_InputAxisEvent") {
                Root r;
                r.node = &n;
                r.start = execOut(n, "then");
                r.kind = "input";
                r.title = "InputAxis_" + n.memberName;
                r.fn = claim(r.title, "InputAxis");
                names.push_back({title(n), r.fn, "input axis event"});
                r.params = paramsOf(n, PinDir::Out);
                std::string axis = map.inputAxis(n.memberName);
                std::string args;
                for (auto& pa : r.params) args += (args.empty() ? "" : ", ") + (pa.ty.k == K::Float ? "Input::Axis(" + cppStringLiteral(axis) + ")" : defaultOf(pa.ty));
                r.dispatch = r.fn + "(" + args + ");  // Unreal axis events run every frame";
                static const std::set<std::string> axes = {"MoveX", "MoveY", "LookX", "LookY"};
                r.sev = axes.contains(axis) ? Severity::Converted : Severity::Approximated;
                if (!axes.contains(axis)) r.sevNote = "add the axis '" + axis + "' to the project's input map";
                addRoot(std::move(r));
            }
        }
    }
    void collectDispatchers() {
        for (auto& v : g.variables)
            if (v.type.category == "mcdelegate" && !dispatchers.contains(v.name)) dispatchers[v.name] = {v.name, cls + "_" + sanitizeIdentifier(v.name, "Event"), {}};
        for (auto& n : g.nodes) {
            bool call = n.cls == "K2Node_CallDelegate";
            bool bind = n.cls == "K2Node_AddDelegate" || n.cls == "K2Node_AssignDelegate" || n.cls == "K2Node_RemoveDelegate" || n.cls == "K2Node_ClearDelegate";
            if (!call && !bind) continue;
            if (n.memberName.empty()) continue;
            auto& d = dispatchers[n.memberName];
            if (d.ue.empty()) d = {n.memberName, cls + "_" + sanitizeIdentifier(n.memberName, "Event"), {}};
            if (call && d.fields.empty())
                for (auto& p : n.pins)
                    if (p.dir == PinDir::In && isDataPin(p) && p.name != "self") d.fields.push_back({p.name, tyOf(p.type)});
        }
        for (auto& n : g.nodes) {  // fields from the bound custom event when no Call node shows them
            if (n.cls != "K2Node_AddDelegate" && n.cls != "K2Node_AssignDelegate") continue;
            auto it = dispatchers.find(n.memberName);
            if (it == dispatchers.end() || !it->second.fields.empty()) continue;
            for (auto& p : n.pins)
                if (p.dir == PinDir::In && p.type.category == "delegate")
                    for (auto& l : linksOf(&p))
                        for (auto& lp : l.n->pins)
                            if (lp.dir == PinDir::Out && isDataPin(lp)) it->second.fields.push_back({lp.name, tyOf(lp.type)});
        }
        for (auto& [k, d] : dispatchers) {
            // the payload struct lives in the header, which cannot see the bp:: helper types
            for (auto& f : d.fields)
                if (cppOf(f.second).find("bp::") != std::string::npos) f.second = Ty{K::Vector};
            d.structName = claim(d.structName, "Event");
            names.push_back({d.ue, d.structName, "event dispatcher (struct)"});
        }
    }
    std::string fieldName(const Dispatcher& d, size_t i) const {
        std::set<std::string> taken{"sender"};
        std::string s;
        for (size_t k = 0; k <= i && k < d.fields.size(); ++k) {
            s = sanitizeIdentifier(d.fields[k].first, "value");
            while (taken.contains(s)) s += "_";
            taken.insert(s);
        }
        return s;
    }
    std::string headerFieldType(const Ty& t) const {  // the header cannot see the bp:: helpers
        std::string c = cppOf(t);
        if (c.find("bp::") != std::string::npos) return "Vec3";
        return c;
    }

    // ------------------------------------------------------------------------------------- generate --
    void reset(const std::string& className) {
        rep = Report{};
        fatal.clear();
        cppText.clear();
        headerText.clear();
        cls = className;
        used.clear();
        names.clear();
        vars.clear();
        varOrder.clear();
        members.clear();
        memberByKey.clear();
        memberTypes.clear();
        memberTys.clear();
        roots.clear();
        rootByName.clear();
        dispatchers.clear();
        publicFns.clear();
        privateFns.clear();
        helperFns.clear();
        helperByKey.clear();
        stubFnByName.clear();
        callbacks.clear();
        startPrologue.clear();
        nodeState.clear();
        unrecognisedSeen.clear();
        statements = evaluations = 0;
        subflowCounter = 0;
        usesMovement = false;
        evalStack.clear();
        used.insert(cls);
    }

    void generate(const std::string& className) {
        reset(className);
        // declared variables first (their defaults and tooltips are known)
        for (auto& v : g.variables) {
            if (v.type.category == "mcdelegate") continue;
            Var& var = variable(v.name, &v.type);
            var.declared = true;
            var.def = v.defaultValue;
            var.tooltip = v.tooltip;
        }
        collectDispatchers();
        collectRoots();
        for (auto& r : roots) r.latent = r.start && r.kind != "function" && reachesLatent(r.start);

        // ---- bodies
        for (auto& r : roots) {
            Out o;
            o.ind = 2;
            Ctx c;
            c.root = &r;
            c.latent = r.latent;
            c.noHelpers = r.latent || r.kind == "function";
            c.fnRoot = r.kind == "function" ? &r : nullptr;
            mark(*r.node, r.sev, r.sevNote);
            if (r.start) emitExecOut(r.start, c, o);
            std::string prologue;
            for (auto& pa : r.params)
                if (mirrorNeeded.contains(pa.pin))
                    prologue += "        " + member("mirror#" + r.node->name + "#" + pa.pin->id, cppOf(pa.ty), shortTitle(*r.node) + "_" + pa.ue) + " = " + pa.name + ";\n";
            std::string sig;
            for (auto& pa : r.params) sig += (sig.empty() ? "" : ", ") + std::string("[[maybe_unused]] ") + cppOf(pa.ty) + " " + pa.name;
            std::string ret = "void";
            if (r.kind == "function") {
                if (r.outs.size() == 1) ret = cppOf(r.outs[0].ty);
                else
                    for (auto& out : r.outs) sig += (sig.empty() ? "" : ", ") + cppOf(out.ty) + "& " + out.name;
            }
            std::string head = "    // Unreal: " + commentText(title(*r.node) + (r.kind == "input" ? " (" + r.start->name + ")" : ""), 100);
            if (r.sev != Severity::Converted && !r.sevNote.empty())
                head += std::string(r.sev == Severity::Stubbed ? " - TODO(blueprint): " : " - approximated: ") + commentText(r.sevNote, 140);
            std::string text = head + "\n";
            if (r.latent) {
                std::string args;
                for (auto& pa : r.params) args += (args.empty() ? "" : ", ") + pa.name;
                std::string flow = claim(r.fn + "_Flow", "Flow");
                text += "    void " + r.fn + "(" + sig + ") { StartCoroutine(" + flow + "(" + args + ")); }\n";
                text += "    Task " + flow + "(" + sig + ") {\n" + prologue + o.s + "        co_return;\n    }\n";
            } else {
                text += "    " + ret + " " + r.fn + "(" + sig + ") {\n" + prologue + o.s;
                if (ret != "void") text += "        return " + defaultOf(r.outs[0].ty) + ";\n";
                text += "    }\n";
            }
            (r.isPublic ? publicFns : privateFns).push_back(text);
            // lifecycle wiring
            if (r.kind == "event" && r.em && !r.em->callback.empty()) {
                std::string a;
                for (auto& pa : r.params) {
                    std::string v;
                    if (auto it = r.em->args.find(pa.ue); it != r.em->args.end()) {
                        v = it->second;
                        bool plain = (pa.ty.k == K::Float && v == "dt") || (pa.ty.k == K::Entity && (v == "other" || v == "c.other" || v == "GetEntity()"));
                        if (!plain) v = "bp::As<" + cppOf(pa.ty) + ">(" + v + ")";
                    } else {
                        v = defaultOf(pa.ty);
                    }
                    a += (a.empty() ? "" : ", ") + v;
                }
                callbacks[r.em->callback].push_back(r.fn + "(" + a + ");");
            } else if (r.kind == "input") {
                callbacks["#input"].push_back(r.dispatch);
            }
        }
        // ---- report: nodes
        for (auto& n : g.nodes) {
            NodeEntry e;
            e.node = n.name.substr(0, 120);
            e.cls = n.cls.substr(0, 80);
            e.title = commentText(title(n), 120);
            e.graph = n.graph.substr(0, 80);
            e.line = n.line;
            auto it = nodeState.find(&n);
            if (n.cls == "EdGraphNode_Comment") {
                e.severity = Severity::Ignored;
                e.detail = "comment box";
            } else if (it == nodeState.end() || !it->second.touched) {
                e.severity = Severity::Ignored;
                e.detail = "not reached from any event or function (unconnected or unused)";
                bool known = isKnownClass(n.cls);
                if (!known) unrecognised("node class", n.cls, &n, "unconnected");
            } else {
                e.severity = it->second.sev;
                e.detail = it->second.detail;
            }
            rep.nodes.push_back(std::move(e));
        }
        assemble();
    }
    static bool isKnownClass(const std::string& c);

    void assemble() {
        const std::string& C = cls;
        // ---- variables
        std::string props, plain;
        for (auto& ue : varOrder) {
            Var& v = vars.at(ue);
            std::string def = v.def.empty() ? defaultOf(v.ty) : literalOf(v.ty, v.def, v.def, v.def);
            if (v.ty.cont) def = defaultOf(v.ty);
            VariableEntry ve{v.ue, v.cpp, v.unrealType, cppOf(v.ty), v.def, propertyOk(v.ty), v.declared};
            rep.variables.push_back(ve);
            std::string tip;
            if (v.cpp != v.ue) tip = "Unreal: " + v.ue;
            if (!v.tooltip.empty()) tip += (tip.empty() ? "" : " - ") + v.tooltip;
            if (propertyOk(v.ty)) {
                props += "    FLEDGE_PROPERTY(" + cppOf(v.ty) + ", " + v.cpp + ", " + def + (tip.empty() ? "" : ", Tooltip(" + cppStringLiteral(tip.substr(0, 200)) + ")") + ");\n";
            } else {
                plain += "    " + cppOf(v.ty) + " " + v.cpp + " = " + def + ";  // Unreal type " + commentText(v.unrealType.empty() ? kindName(v.ty.k) : v.unrealType, 60) +
                         " (not shown in the Inspector)\n";
                if (v.ty.k == K::Opaque) unrecognised("variable type", v.ue + " : " + v.unrealType, nullptr, "no Fledge equivalent; a placeholder member was generated");
            }
        }
        // ---- lifecycle
        std::string life;
        auto cb = [&](const std::string& name) -> std::vector<std::string> {
            auto it = callbacks.find(name);
            return it == callbacks.end() ? std::vector<std::string>{} : it->second;
        };
        auto block = [&](const std::string& sig, const std::vector<std::string>& lines) {
            if (lines.empty()) return;
            life += "    " + sig + " override {\n";
            for (auto& l : lines) life += "        " + l + "\n";
            life += "    }\n";
        };
        std::vector<std::string> start = startPrologue;
        for (auto& l : cb("OnStart")) start.push_back(l);
        block("void OnCreate()", cb("OnCreate"));
        block("void OnStart()", start);
        std::vector<std::string> update = cb("#input");
        for (auto& l : cb("OnUpdate")) update.push_back(l);
        if (usesMovement) {
            std::string acc = memberByKey["movement#input"];
            update.push_back("if (" + acc + ".LengthSquared() > 1.0f) " + acc + " = " + acc + ".Normalized();");
            update.push_back("CharacterController(GetEntity()).Move(" + acc + " * bp::kWalkSpeed);  // AddMovementInput (approximated)");
            update.push_back(acc + " = Vec3();");
        }
        if (!update.empty()) {
            life += "    void OnUpdate([[maybe_unused]] float dt) override {\n";
            for (auto& l : update) life += "        " + l + "\n";
            life += "    }\n";
        }
        auto withArg = [&](const char* name, const char* sig) {
            auto lines = cb(name);
            if (lines.empty()) return;
            life += std::string("    ") + sig + " override {\n";
            for (auto& l : lines) life += "        " + l + "\n";
            life += "    }\n";
        };
        withArg("OnLateUpdate", "void OnLateUpdate([[maybe_unused]] float dt)");
        withArg("OnFixedUpdate", "void OnFixedUpdate([[maybe_unused]] float dt)");
        block("void OnEnable()", cb("OnEnable"));
        block("void OnDisable()", cb("OnDisable"));
        block("void OnDestroy()", cb("OnDestroy"));
        withArg("OnTriggerEnter", "void OnTriggerEnter([[maybe_unused]] Entity other)");
        withArg("OnTriggerExit", "void OnTriggerExit([[maybe_unused]] Entity other)");
        withArg("OnCollisionEnter", "void OnCollisionEnter([[maybe_unused]] const Collision& c)");
        withArg("OnCollisionExit", "void OnCollisionExit([[maybe_unused]] const Collision& c)");

        // ---- header (event dispatcher payloads)
        if (!dispatchers.empty()) {
            std::string h = "// Event dispatcher payloads of " + C + " (generated by the Fledge Blueprint converter).\n";
            h += "// Other scripts can listen with Subscribe<" + dispatchers.begin()->second.structName + ">([](const auto& e) { ... });\n";
            h += "#pragma once\n#include <fledge/script/script.h>\n\n#include <map>\n#include <string>\n#include <vector>\n\n";
            for (auto& [k, d] : dispatchers) {
                h += "// Unreal event dispatcher: " + commentText(d.ue, 80) + "\n";
                h += "struct " + d.structName + " {\n    fledge::script::Entity sender;\n";
                for (size_t i = 0; i < d.fields.size(); ++i) {
                    std::string ft = headerFieldType(d.fields[i].second), full;
                    for (size_t p = 0; p < ft.size();) {  // qualify the SDK type names (the header has no using-directive)
                        size_t q = p;
                        while (q < ft.size() && (std::isalnum(static_cast<unsigned char>(ft[q])) || ft[q] == '_' || ft[q] == ':')) ++q;
                        if (q == p) {
                            full += ft[p++];
                            continue;
                        }
                        std::string word = ft.substr(p, q - p);
                        full += (word == "Vec3" || word == "Color" || word == "Entity" || word == "TimerHandle") ? "fledge::script::" + word : word;
                        p = q;
                    }
                    h += "    " + full + " " + fieldName(d, i) + "{};\n";
                }
                h += "};\n";
            }
            headerText = h;
        }
        // ---- counts for the header comment
        int conv = 0, appr = 0, stub = 0;
        for (auto& e : rep.nodes) {
            if (e.severity == Severity::Converted) ++conv;
            else if (e.severity == Severity::Approximated) ++appr;
            else if (e.severity == Severity::Stubbed) ++stub;
        }
        std::string s;
        s += "// " + C + ": Fledge Script behaviour converted from an Unreal Engine Blueprint by the Fledge Blueprint converter";
        s += opt.fledgeVersion.empty() ? std::string(".\n") : " (Fledge " + commentText(opt.fledgeVersion, 20) + ").\n";
        s += "// Source: " + commentText(opt.sourceName.empty() ? std::string("pasted text") : opt.sourceName, 120) + "\n";
        s += "// Nodes: " + std::to_string(conv) + " converted, " + std::to_string(appr) + " approximated, " + std::to_string(stub) +
             " stubbed. Search for TODO(blueprint) and read the conversion report before shipping.\n";
        s += "// Coordinates: " + std::string(opt.unrealSpace ? "Unreal space (cm, X forward, Z up) is converted to Fledge space (m, -Z forward, Y up) where the Blueprint talks to the engine (bp::PosFromUE ...)."
                                                            : "values pass through unchanged (unrealSpace = false).") + "\n";
        s += "// " + commentText("Licence: Blueprints and assets from the Unreal Marketplace / Fab or other Epic content may be licensed for use only in Unreal Engine.", 200) + "\n";
        s += "// " + commentText("You are responsible for having the right to convert and use this logic outside Unreal.", 200) + "\n";
        s += "#include <fledge/script/script.h>\n\n#include <algorithm>\n#include <cctype>\n#include <cmath>\n#include <cstdlib>\n#include <map>\n#include <string>\n#include <utility>\n#include <vector>\n";
        if (!headerText.empty()) s += "\n#include \"" + C + ".h\"\n";
        s += "\nusing namespace fledge::script;\n\nnamespace {\n";
        std::string pre = kPrelude;
        size_t at = pre.find("@UNREAL_SPACE@");
        if (at != std::string::npos) pre.replace(at, 14, opt.unrealSpace ? "true" : "false");
        s += pre;
        s += "} // namespace\n\n";
        s += "class " + C + " : public Behaviour {\npublic:\n";
        if (!props.empty()) s += "    // ---- Blueprint variables (Inspector, saved in the scene)\n" + props;
        if (!life.empty()) s += "\n    // ---- Lifecycle (Unreal events -> Fledge callbacks)\n" + life;
        if (!publicFns.empty()) {
            s += "\n    // ---- Custom events and functions (callable from other scripts)\n";
            for (auto& f : publicFns) s += f;
        }
        s += "\nprivate:\n";
        if (!plain.empty()) s += "    // ---- Blueprint variables without an Inspector type\n" + plain;
        if (!privateFns.empty()) {
            s += "\n    // ---- Event graphs\n";
            for (auto& f : privateFns) s += f;
        }
        if (!helperFns.empty()) {
            s += "\n    // ---- Flows shared by several wires\n";
            for (auto& f : helperFns) s += f;
        }
        if (!members.empty()) {
            s += "\n    // ---- Node state (Blueprint values persist after a node ran)\n";
            for (auto& m : members)
                s += "    " + m.type + " " + m.name + (m.init.empty() ? std::string("{}") : (m.init.front() == '{' ? m.init : "{" + m.init + "}")) + ";" +
                     (m.comment.empty() ? "" : "  // " + commentText(m.comment, 100)) + "\n";
        }
        s += "};\nFLEDGE_BEHAVIOUR(" + C + ")\n";
        cppText = std::move(s);
        rep.names = names;
        rep.className = C;
        rep.sourceName = commentText(opt.sourceName.empty() ? std::string("pasted text") : opt.sourceName, 200);
        rep.fledgeVersion = opt.fledgeVersion;
    }
};

} // namespace

bool isKnownNodeClass(std::string_view cls);  // t3d.cpp
bool Emitter::isKnownClass(const std::string& c) { return isKnownNodeClass(c); }

// ------------------------------------------------------------------------------------------ public --
const char* toString(Severity s) {
    switch (s) {
        case Severity::Converted: return "converted";
        case Severity::Approximated: return "approximated";
        case Severity::Stubbed: return "stubbed";
        case Severity::Ignored: return "ignored";
    }
    return "?";
}

namespace {
std::string defaultClassName(const Graph& g, const Options& o) {
    if (!o.className.empty()) return o.className;
    if (!g.blueprintName.empty()) return g.blueprintName;
    std::string s = o.sourceName;
    if (size_t d = s.find_last_of("/\\"); d != std::string::npos) s = s.substr(d + 1);
    size_t dot = s.find('.');
    if (dot == std::string::npos) return "ConvertedBlueprint";  // "pasted text": not a file name
    s = s.substr(0, dot);
    return s.empty() ? std::string("ConvertedBlueprint") : s;
}
} // namespace

Conversion convertText(std::string_view text, const Options& options) {
    Conversion c;
    c.report.sourceName = commentText(options.sourceName.empty() ? std::string("pasted text") : options.sourceName, 200);
    c.report.sourceBytes = text.size();
    c.report.fledgeVersion = options.fledgeVersion;
    if (text.size() > options.limits.maxInputBytes) {
        c.report.error = "the input is " + std::to_string(text.size() / 1024) + " KiB; the limit is " + std::to_string(options.limits.maxInputBytes / 1024) + " KiB";
        return c;
    }
    Graph g = parseGraph(text, options.limits);
    std::string cls = sanitizeIdentifier(defaultClassName(g, options), "ConvertedBlueprint");
    if (g.nodes.empty()) {
        c.className = cls;
        c.report.className = cls;
        c.report.error = "no Blueprint nodes found. Select nodes in the Unreal Blueprint editor, press Ctrl+C and paste the text here "
                         "(it starts with 'Begin Object Class=/Script/BlueprintGraph.K2Node_...').";
        for (auto& i : g.issues) c.report.warnings.push_back("line " + std::to_string(i.line) + ": " + commentText(i.message, 200));
        return c;
    }
    const MappingTable& table = options.mappings ? *options.mappings : MappingTable::builtin();
    Emitter e(g, options, table);
    c = e.run(cls);
    c.report.sourceBytes = text.size();
    c.report.sourceKind = "t3d";
    if (g.truncated) c.report.warnings.insert(c.report.warnings.begin(), "the input was truncated or hit a limit: some nodes were not read");
    for (auto& i : g.issues) {
        bool unknownProp = i.message.rfind("unrecognised", 0) == 0;
        if (unknownProp) c.report.unrecognised.push_back({"text", commentText(i.message, 200), "", "", i.line});
        else c.report.warnings.push_back("line " + std::to_string(i.line) + ": " + commentText(i.message, 200));
    }
    for (auto& o : g.ignoredObjects) c.report.unrecognised.push_back({"object", commentText(o, 160), "", "not a Blueprint node; skipped", 0});
    return c;
}

} // namespace fledge::blueprint
