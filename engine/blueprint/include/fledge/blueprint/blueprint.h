#pragma once
// Unreal Engine Blueprint -> Fledge Script (C++) converter (editor / tools only; docs/BLUEPRINT_CONVERTER.md).
//
//   T3D clipboard text (Ctrl+C on nodes in the Blueprint editor)  --parseT3D-->  raw objects
//     --buildGraph-->  typed graph IR (nodes, pins, links, variables, graphs)
//     --convert-->  one Fledge Script behaviour (.cpp, plus a .h only for event dispatcher structs) + a conversion report
//
// Safety: the input is DATA. Nothing in it is executed, nothing is fetched (no network, no Epic services), input size and
// graph size / nesting are limited, and every identifier, string literal and comment that reaches the generated C++ is
// sanitised or escaped (no code injection through pin names or default values). Unknown nodes, pins and properties become
// report entries, never exceptions; the generated file always compiles (unmapped nodes become typed TODO stubs).
//
// Honest status: the parser was written from knowledge of the K2Node T3D format and tested on hand-written samples modelled
// on it (tests/data/blueprints); it still needs validation against real exports. .uasset support is an inventory probe only.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge::blueprint {

// Shown once in the editor panel, in every report and in docs/BLUEPRINT_CONVERTER.md.
inline constexpr const char* kLicenceNotice =
    "Licence: Blueprints and assets from the Unreal Marketplace / Fab or other Epic content may be licensed for use only inside "
    "Unreal Engine. You are responsible for having the right to convert and use them outside Unreal. Fledge does not fetch "
    "anything from Epic: the converter only processes the text or file you give it, makes no network calls and executes "
    "nothing from the input.";

struct Limits {
    size_t maxInputBytes = 8u * 1024 * 1024;  // 8 MiB of T3D text / .uasset
    size_t maxNodes = 20000;
    size_t maxPinsPerNode = 1024;
    int maxObjectNesting = 32;     // Begin Object ... End Object
    int maxValueNesting = 48;      // ( ... ) inside a property value
    int maxExpressionDepth = 64;   // pure-node expression nesting
    int maxControlDepth = 96;      // nested branches / loops in one flow
    size_t maxEmittedStatements = 60000;  // after expanding merged flows
};

// ------------------------------------------------------------------ raw T3D --
struct RawObject {
    std::string cls;   // "/Script/BlueprintGraph.K2Node_CallFunction" (may be empty)
    std::string name;  // "K2Node_CallFunction_3"
    int line = 0;
    std::vector<std::pair<std::string, std::string>> props;  // key -> raw value text (in file order)
    std::vector<std::string> pins;          // CustomProperties Pin (...) bodies (inside the outer parentheses)
    std::vector<std::string> userPins;      // CustomProperties UserDefinedPin (...)
    std::vector<RawObject> children;
};
struct ParseIssue {
    int line = 0;
    std::string message;
};
struct RawParse {
    std::vector<RawObject> objects;  // top level
    std::vector<ParseIssue> issues;
    bool truncated = false;          // a limit stopped parsing early
};
RawParse parseT3D(std::string_view text, const Limits& limits = {});

// Parses one T3D struct value "(A=1,B="x",C=(D=2),E=(N1 P1,N2 P2,))" into key -> raw value. Tolerant: never throws.
std::vector<std::pair<std::string, std::string>> parseStruct(std::string_view value, int maxDepth = 48);
// "\"abc\"" -> abc (T3D escapes \" \\ \n \r \t); unquoted values are returned trimmed.
std::string unquote(std::string_view v);

// ------------------------------------------------------------------ graph IR --
enum class PinDir { In, Out };
struct PinType {
    std::string category;        // exec, bool, int, int64, byte, real, float, double, string, name, text, struct, object, class, ...
    std::string subCategory;     // "double" / "float" for real, "self", ...
    std::string subObject;       // short name of PinSubCategoryObject: "Vector", "Actor", "SoundBase", ...
    std::string container;       // "", "Array", "Set", "Map"
    std::string valueCategory, valueSubObject;  // Map value type
    bool isReference = false;
};
struct Link {
    std::string node, pinId;
};
struct Pin {
    std::string id, name, friendlyName;
    PinDir dir = PinDir::In;
    PinType type;
    std::string defaultValue, defaultObject, defaultText;
    bool hasDefault = false, hidden = false;
    std::string parentPin;               // split struct pins: the parent's id
    std::vector<std::string> subPins;    // split struct pins: the children's ids
    std::vector<Link> links;
};
struct Node {
    std::string name, cls, classPath;  // cls = short class ("K2Node_IfThenElse")
    std::string graph;                 // owning EdGraph name when nested ("" for a flat clipboard paste)
    int line = 0;
    std::map<std::string, std::string> props;
    std::vector<Pin> pins;
    // decoded member reference (FunctionReference / EventReference / VariableReference / DelegateReference)
    std::string memberParent, memberName;
    bool selfContext = false;
    std::string comment;  // NodeComment
    const Pin* pin(std::string_view name, std::optional<PinDir> dir = std::nullopt) const;
    const Pin* pinById(std::string_view id) const;
    bool hasExecInput() const;
};
struct Variable {
    std::string name;
    PinType type;
    std::string defaultValue, tooltip;
    bool declared = false;  // from NewVariables (whole-Blueprint export), not only inferred from Get / Set nodes
};
struct Graph {
    std::string blueprintName;  // Blueprint object name when present
    std::vector<Node> nodes;
    std::vector<Variable> variables;  // declared ones (NewVariables)
    std::vector<std::string> graphNames;
    std::vector<ParseIssue> issues;
    std::vector<std::string> ignoredObjects;  // non-node objects that were skipped
    bool truncated = false;
    const Node* find(std::string_view name) const;
};
Graph buildGraph(const RawParse& raw, const Limits& limits = {});
Graph parseGraph(std::string_view text, const Limits& limits = {});

// ------------------------------------------------------------- mapping table --
// Data-driven mapping of Unreal functions / events / input names to Fledge Script (engine/blueprint/data/node_mappings.json,
// embedded at build time; a project may add <project>/blueprint_mappings.json, merged over it).
struct FunctionMapping {
    std::string name, parent;  // MemberName, optional MemberParent class (short name)
    std::string status = "converted";  // converted | approximated | stubbed
    std::string note;
    std::string handler;       // built-in handler for nodes that need more than a template ("delay", "timer_by_name", ...)
    std::string stmt;          // impure: statement template
    std::string expr;          // pure: ReturnValue expression template
    std::string op;            // binary operator for variadic nodes ("+", "&&", ...)
    std::map<std::string, std::string> args;     // pin -> type name
    std::map<std::string, std::string> outputs;  // output pin -> expression template
    std::map<std::string, std::string> outTypes; // output pin -> type name
    std::map<std::string, std::string> pinAliases;  // template pin -> pin name used by other engine versions
    std::vector<std::string> aliases;              // other MemberNames (UE4 "Add_FloatFloat" for UE5 "Add_DoubleDouble")
    bool pure = false;
};
struct EventMapping {
    std::string name;      // EventReference MemberName ("ReceiveTick")
    std::string callback;  // OnStart | OnUpdate | OnLateUpdate | OnFixedUpdate | OnDestroy | OnTriggerEnter | ... | "" (none)
    std::string title;     // readable name for the generated function ("Tick")
    std::string status = "converted", note;
    std::map<std::string, std::string> args;  // event output pin -> expression from the callback's parameters
};
class MappingTable {
public:
    static const MappingTable& builtin();
    static Result<MappingTable> fromJson(const Json& j);  // validates
    void merge(const MappingTable& other);                // other wins
    const FunctionMapping* function(std::string_view member, std::string_view parent = {}) const;
    const EventMapping* event(std::string_view member) const;
    std::string inputAction(std::string_view unrealName) const;  // mapped or the name itself
    std::string inputAxis(std::string_view unrealName) const;
    std::string key(std::string_view unrealKey) const;           // "" when unknown
    size_t functionCount() const { return m_functions.size(); }
    size_t eventCount() const { return m_events.size(); }
    const std::vector<FunctionMapping>& functions() const { return m_functions; }
    const std::vector<EventMapping>& events() const { return m_events; }
    Json toJson() const;
    int version = 1;

private:
    std::vector<FunctionMapping> m_functions;
    std::vector<EventMapping> m_events;
    std::map<std::string, size_t> m_byName;  // lower(name) -> index (aliases included)
    std::map<std::string, std::string> m_actions, m_axes, m_keys;
};
const char* builtinMappingJson();

// --------------------------------------------------------------- conversion --
struct Options {
    std::string className;      // "" = from the Blueprint / source name
    std::string sourceName;     // file name or "pasted text" (only used in comments and the report, sanitised)
    bool unrealSpace = true;    // convert Unreal cm / X-forward Z-up to Fledge metres / -Z-forward Y-up at API boundaries
    Limits limits;
    const MappingTable* mappings = nullptr;  // nullptr = builtin()
    std::string fledgeVersion;  // for the header comment / report
};

enum class Severity { Converted, Approximated, Stubbed, Ignored };
const char* toString(Severity s);

struct NodeEntry {
    std::string node, cls, title, graph, detail;
    Severity severity = Severity::Converted;
    int line = 0;
};
struct Unrecognised {
    std::string construct;  // "node class", "function", "pin property", "macro", "event", ...
    std::string name, where, detail;
    int line = 0;
};
struct VariableEntry {
    std::string name, cppName, unrealType, cppType, defaultValue;
    bool property = false;  // FLEDGE_PROPERTY (Inspector, saved in the scene)
    bool declared = false;
};
struct NameEntry {
    std::string unreal, cpp, kind;
};
struct Report {
    std::string sourceName, sourceKind = "t3d", className;
    size_t sourceBytes = 0;
    std::vector<std::string> files;
    std::vector<NodeEntry> nodes;
    std::vector<Unrecognised> unrecognised;
    std::vector<VariableEntry> variables;
    std::vector<NameEntry> names;
    std::vector<std::string> warnings;
    std::string error;            // fatal error (nothing generated)
    std::string fledgeVersion;
    Json uasset;                  // .uasset probe details (sourceKind == "uasset")
    int count(Severity s) const;
    Json toJson() const;          // schema "fledge.blueprint-report/1" (strings forced to valid UTF-8)
    std::string toMarkdown() const;
    std::string summary() const;  // one line

private:
    Json toJsonRaw() const;
};

struct Conversion {
    bool ok = false;
    std::string className;
    std::string cppFile, cpp;      // "<Class>.cpp" + source
    std::string headerFile, header;  // only when needed ("" otherwise)
    Report report;
};

Conversion convertText(std::string_view t3dText, const Options& options = {});
// .t3d / .txt / .copy -> convertText; .uasset / .umap -> probe only (report, no code). Size limited before reading.
Conversion convertFile(const fs::path& file, const Options& options = {});
// Reads a T3D text file (UTF-8, or UTF-16 with a BOM) within the size limit; fails for Unreal packages and missing files.
Result<std::string> readT3DText(const fs::path& file, const Limits& limits = {});
bool isUnrealPackage(std::string_view bytes);

// ------------------------------------------------------------------ uasset --
struct UassetProbe {
    bool isPackage = false, cooked = false, unversioned = false, headerParsed = false;
    int32_t legacyVersion = 0, ue4Version = 0, ue5Version = 0, licenseeVersion = 0;
    uint32_t packageFlags = 0;
    int32_t nameCount = 0;
    std::vector<std::string> names;            // name table (or heuristic strings)
    std::vector<std::string> blueprintNodes;   // K2Node_* classes found
    std::vector<std::string> functionsFound;   // names matching the mapping table
    std::string method;                        // "name table" | "string scan" | "none"
    std::string message;                       // what is / is not supported
    Json toJson() const;
};
UassetProbe probeUasset(std::string_view bytes, const MappingTable& table = MappingTable::builtin());

// ------------------------------------------------------------------ helpers --
// C++ identifier from arbitrary text: [A-Za-z_][A-Za-z0-9_]*, no keywords / reserved names / SDK names, <= 64 chars.
std::string sanitizeIdentifier(std::string_view text, std::string_view fallback = "Value");
// A valid, safe C++ string literal (with the quotes) for any bytes: escapes quotes, backslashes, control and non-ASCII bytes.
std::string cppStringLiteral(std::string_view text);
// Text that is safe inside a // comment: printable ASCII, no backslash (line splicing), no "*/", <= maxLen.
std::string commentText(std::string_view text, size_t maxLen = 160);
bool isReservedIdentifier(std::string_view id);

// ------------------------------------------------------------------ output --
struct WrittenFiles {
    std::string className;
    fs::path cpp, header, reportMarkdown, reportJson;
    std::vector<std::string> notes;
};
// Picks a class / file name that does not exist yet (<Class>, <Class>_2, ...) under scriptsDir/folder and is not already
// registered by another script in scriptsDir (FLEDGE_BEHAVIOUR / FLEDGE_SERVICE).
std::string uniqueClassName(const fs::path& scriptsDir, const std::string& folder, const std::string& wanted);
// Re-emits the conversion under `className` (same options); used after uniqueClassName picked a suffix.
Conversion renamed(const Conversion& c, std::string_view t3dText, const Options& options, const std::string& className);
// Writes the .cpp (and .h) into scriptsDir/folder and the report (.md + .json) into reportDir. Never overwrites: fails if a
// target exists (call uniqueClassName first).
Result<WrittenFiles> writeConversion(const Conversion& c, const fs::path& scriptsDir, const std::string& folder, const fs::path& reportDir);
// Validates a Scripts/-relative folder ("" or "Converted/Enemies"): relative, no "..", no drive / UNC, safe characters.
bool isSafeSubfolder(std::string_view folder);

} // namespace fledge::blueprint
