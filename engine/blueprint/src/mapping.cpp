// The data-driven node mapping table (data/node_mappings.json, embedded) and its validation.
#include <cctype>
#include <set>

#include "fledge/blueprint/blueprint.h"
#include "fledge/blueprint/node_mappings_json.h"

namespace fledge::blueprint {

const char* builtinMappingJson() { return data::kNodeMappings; }

namespace {
const std::set<std::string>& typeNames() {
    static const std::set<std::string> t = {"bool", "int", "int64", "float", "string", "name", "text", "vector", "rotator", "transform",
                                            "entity", "class", "asset", "timer", "color", "any"};
    return t;
}
bool validTypeName(const std::string& t) {
    if (typeNames().contains(t)) return true;
    for (const char* c : {"array:", "set:", "map:"})
        if (startsWith(t, c)) return typeNames().contains(t.substr(std::string(c).size()));
    return false;
}
const std::set<std::string>& callbacks() {
    static const std::set<std::string> c = {"", "OnCreate", "OnStart", "OnUpdate", "OnLateUpdate", "OnFixedUpdate", "OnDestroy", "OnEnable",
                                            "OnDisable", "OnTriggerEnter", "OnTriggerExit", "OnCollisionEnter", "OnCollisionExit"};
    return c;
}
const std::set<std::string>& handlers() {
    static const std::set<std::string> h = {"delay", "retriggerable_delay", "next_tick", "timer_by_name", "timer_by_event",
                                            "clear_timer_by_name", "movement"};
    return h;
}
// ${...} placeholders must be well formed; the text between them is C++ written by whoever maintains the table.
std::string checkTemplate(const std::string& t) {
    for (size_t i = t.find("${"); i != std::string::npos; i = t.find("${", i + 2)) {
        size_t e = t.find('}', i);
        if (e == std::string::npos) return "unterminated ${";
        std::string tok = t.substr(i + 2, e - i - 2);
        if (tok.empty() || tok.size() > 64) return "empty or over-long placeholder";
        for (char c : tok)
            if (c == '{' || c == '$' || c == '\n') return "bad placeholder '" + tok + "'";
    }
    return {};
}
std::map<std::string, std::string> stringMap(const Json& j) {
    std::map<std::string, std::string> m;
    if (j.is_object())
        for (auto& [k, v] : j.items())
            if (v.is_string()) m[k] = v.get<std::string>();
    return m;
}
} // namespace

Result<MappingTable> MappingTable::fromJson(const Json& j) {
    MappingTable t;
    if (!j.is_object()) return Result<MappingTable>::failure("mapping table: not a JSON object");
    t.version = j.value("version", 1);
    if (j.contains("functions") && !j["functions"].is_array()) return Result<MappingTable>::failure("mapping table: 'functions' must be an array");
    if (j.contains("events") && !j["events"].is_array()) return Result<MappingTable>::failure("mapping table: 'events' must be an array");
    for (auto& f : j.value("functions", Json::array())) {
        if (!f.is_object() || !f.contains("name") || !f["name"].is_string()) return Result<MappingTable>::failure("mapping table: a function entry has no name");
        FunctionMapping m;
        m.name = f["name"].get<std::string>();
        m.parent = f.value("parent", "");
        m.status = f.value("status", "converted");
        m.note = f.value("note", "");
        m.handler = f.value("handler", "");
        m.stmt = f.value("stmt", "");
        m.expr = f.value("expr", "");
        m.op = f.value("op", "");
        m.pure = f.value("pure", false);
        m.args = stringMap(f.value("args", Json::object()));
        m.outputs = stringMap(f.value("outputs", Json::object()));
        m.outTypes = stringMap(f.value("outTypes", Json::object()));
        m.pinAliases = stringMap(f.value("pinAliases", Json::object()));
        for (auto& a : f.value("aliases", Json::array()))
            if (a.is_string()) m.aliases.push_back(a.get<std::string>());
        std::string where = "mapping '" + m.name + "': ";
        if (m.status != "converted" && m.status != "approximated" && m.status != "stubbed") return Result<MappingTable>::failure(where + "bad status");
        if (!m.handler.empty() && !handlers().contains(m.handler)) return Result<MappingTable>::failure(where + "unknown handler '" + m.handler + "'");
        if (m.handler.empty() && m.stmt.empty() && m.expr.empty() && m.op.empty() && m.outputs.empty())
            return Result<MappingTable>::failure(where + "needs stmt, expr, op, outputs or handler");
        if (!m.op.empty() && m.op != "+" && m.op != "*" && m.op != "&&" && m.op != "||") return Result<MappingTable>::failure(where + "op must be + * && ||");
        if (m.pure && !m.stmt.empty()) return Result<MappingTable>::failure(where + "a pure mapping cannot have a stmt");
        for (auto& [k, v] : m.args)
            if (!validTypeName(v)) return Result<MappingTable>::failure(where + "bad type '" + v + "' for " + k);
        for (auto& [k, v] : m.outTypes)
            if (!validTypeName(v)) return Result<MappingTable>::failure(where + "bad output type '" + v + "' for " + k);
        for (const std::string* s : {&m.stmt, &m.expr})
            if (auto e = checkTemplate(*s); !e.empty()) return Result<MappingTable>::failure(where + e);
        for (auto& [k, v] : m.outputs)
            if (auto e = checkTemplate(v); !e.empty()) return Result<MappingTable>::failure(where + e);
        t.m_functions.push_back(std::move(m));
    }
    for (auto& e : j.value("events", Json::array())) {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) return Result<MappingTable>::failure("mapping table: an event entry has no name");
        EventMapping m;
        m.name = e["name"].get<std::string>();
        m.callback = e.value("callback", "");
        m.title = e.value("title", m.name);
        m.status = e.value("status", "converted");
        m.note = e.value("note", "");
        m.args = stringMap(e.value("args", Json::object()));
        if (!callbacks().contains(m.callback)) return Result<MappingTable>::failure("event '" + m.name + "': unknown callback '" + m.callback + "'");
        if (m.status != "converted" && m.status != "approximated" && m.status != "stubbed") return Result<MappingTable>::failure("event '" + m.name + "': bad status");
        t.m_events.push_back(std::move(m));
    }
    t.m_actions = stringMap(j.value("inputActions", Json::object()));
    t.m_axes = stringMap(j.value("inputAxes", Json::object()));
    t.m_keys = stringMap(j.value("keys", Json::object()));
    for (size_t i = 0; i < t.m_functions.size(); ++i) {
        t.m_byName[toLower(t.m_functions[i].name)] = i;
        for (auto& a : t.m_functions[i].aliases) t.m_byName[toLower(a)] = i;
    }
    return t;
}

const MappingTable& MappingTable::builtin() {
    static const MappingTable t = [] {
        Json j = Json::parse(builtinMappingJson(), nullptr, false);
        auto r = fromJson(j);
        return r ? *r : MappingTable{};  // a broken table is caught by the unit tests (mapping-table sanity)
    }();
    return t;
}

void MappingTable::merge(const MappingTable& o) {
    for (auto& f : o.m_functions) {
        auto it = m_byName.find(toLower(f.name));
        if (it != m_byName.end()) m_functions[it->second] = f;
        else m_functions.push_back(f);
    }
    for (auto& e : o.m_events) {
        bool done = false;
        for (auto& mine : m_events)
            if (mine.name == e.name) {
                mine = e;
                done = true;
            }
        if (!done) m_events.push_back(e);
    }
    for (auto& [k, v] : o.m_actions) m_actions[k] = v;
    for (auto& [k, v] : o.m_axes) m_axes[k] = v;
    for (auto& [k, v] : o.m_keys) m_keys[k] = v;
    m_byName.clear();
    for (size_t i = 0; i < m_functions.size(); ++i) {
        m_byName[toLower(m_functions[i].name)] = i;
        for (auto& a : m_functions[i].aliases) m_byName[toLower(a)] = i;
    }
}

const FunctionMapping* MappingTable::function(std::string_view member, std::string_view parent) const {
    auto it = m_byName.find(toLower(member));
    if (it == m_byName.end()) return nullptr;
    const FunctionMapping& f = m_functions[it->second];
    if (!f.parent.empty() && !parent.empty() && toLower(f.parent) != toLower(parent)) return nullptr;
    return &f;
}
const EventMapping* MappingTable::event(std::string_view member) const {
    for (auto& e : m_events)
        if (toLower(e.name) == toLower(member)) return &e;
    return nullptr;
}
std::string MappingTable::inputAction(std::string_view n) const {
    auto it = m_actions.find(std::string(n));
    return it == m_actions.end() ? std::string(n) : it->second;
}
std::string MappingTable::inputAxis(std::string_view n) const {
    auto it = m_axes.find(std::string(n));
    return it == m_axes.end() ? std::string(n) : it->second;
}
std::string MappingTable::key(std::string_view k) const {
    auto it = m_keys.find(std::string(k));
    if (it != m_keys.end()) return it->second;
    if (k.size() == 1 && std::isalnum(static_cast<unsigned char>(k[0]))) return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(k[0]))));
    if (k.size() >= 2 && k.size() <= 3 && (k[0] == 'F') && std::isdigit(static_cast<unsigned char>(k[1]))) return std::string(k);
    return {};
}
Json MappingTable::toJson() const {
    Json f = Json::array(), e = Json::array();
    for (auto& m : m_functions)
        f.push_back({{"name", m.name}, {"status", m.status}, {"pure", m.pure}, {"handler", m.handler}, {"note", m.note}, {"aliases", m.aliases}});
    for (auto& m : m_events) e.push_back({{"name", m.name}, {"callback", m.callback}, {"status", m.status}, {"note", m.note}});
    return {{"version", version}, {"functions", f}, {"events", e}, {"inputActions", m_actions}, {"inputAxes", m_axes}, {"keys", m_keys}};
}

} // namespace fledge::blueprint
