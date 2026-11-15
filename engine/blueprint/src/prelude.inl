// Support code emitted into every converted file (anonymous namespace, so several converted files never clash).
// @UNREAL_SPACE@ is replaced by true / false (Options::unrealSpace).
R"BPPRE(namespace bp {
// Unreal space (cm, X forward, Y right, Z up) <-> Fledge space (m, -Z forward, +X right, +Y up), applied where the
// Blueprint talks to the engine. false = values pass through unchanged.
[[maybe_unused]] constexpr bool kUnrealSpace = @UNREAL_SPACE@;
[[maybe_unused]] constexpr float kUnitsPerMeter = kUnrealSpace ? 100.0f : 1.0f;
[[maybe_unused]] constexpr int kMaxLoopIterations = 1000000;  // Unreal's default runaway-loop limit
[[maybe_unused]] constexpr float kWalkSpeed = 6.0f;           // m/s: Unreal's default MaxWalkSpeed (600 cm/s)

struct Rotator {  // degrees, like FRotator
    float pitch = 0, yaw = 0, roll = 0;
    bool operator==(const Rotator&) const = default;
};
struct Transform {
    Vec3 location;
    Rotator rotation;
    Vec3 scale{1, 1, 1};
    bool operator==(const Transform&) const = default;
};
struct Opaque {  // an Unreal type with no Fledge equivalent (placeholder; see the conversion report)
    bool operator==(const Opaque&) const = default;
};
template <typename T> T Stub() { return T{}; }  // TODO(blueprint) placeholder value

inline Vec3 PosFromUE(Vec3 v) { return kUnrealSpace ? Vec3(v.y, v.z, -v.x) * 0.01f : v; }
inline Vec3 PosToUE(Vec3 v) { return kUnrealSpace ? Vec3(-v.z, v.x, v.y) * 100.0f : v; }
inline Vec3 DirFromUE(Vec3 v) { return kUnrealSpace ? Vec3(v.y, v.z, -v.x) : v; }
inline Vec3 DirToUE(Vec3 v) { return kUnrealSpace ? Vec3(-v.z, v.x, v.y) : v; }
inline Vec3 ScaleFromUE(Vec3 s) { return kUnrealSpace ? Vec3(s.y, s.z, s.x) : s; }
inline Vec3 ScaleToUE(Vec3 s) { return kUnrealSpace ? Vec3(s.z, s.x, s.y) : s; }
inline Quat RotatorToQuat(Rotator r) {
    return kUnrealSpace ? Quat::FromEuler(Radians(r.pitch), Radians(-r.yaw), Radians(-r.roll))
                        : Quat::FromEuler(Radians(r.pitch), Radians(r.yaw), Radians(r.roll));
}
inline Rotator QuatToRotator(Quat q) {
    Vec3 f = q.Rotate(Vec3(0, 0, -1));
    float yaw = std::atan2(-f.x, -f.z), pitch = std::asin(Clamp(f.y, -1.0f, 1.0f));
    Quat rest = Quat::FromEuler(pitch, yaw, 0).Conjugate() * q;
    float roll = 2.0f * std::atan2(rest.z, rest.w);
    return kUnrealSpace ? Rotator{Degrees(pitch), -Degrees(yaw), -Degrees(roll)} : Rotator{Degrees(pitch), Degrees(yaw), Degrees(roll)};
}
inline Vec3 RotatorForward(Rotator r) { return DirToUE(RotatorToQuat(r).Rotate(Vec3::Forward())); }
inline Vec3 RotatorRight(Rotator r) { return DirToUE(RotatorToQuat(r).Rotate(Vec3::Right())); }
inline Vec3 RotatorUp(Rotator r) { return DirToUE(RotatorToQuat(r).Rotate(Vec3::Up())); }
inline Vec3 RotateVector(Vec3 v, Rotator r) { return DirToUE(RotatorToQuat(r).Rotate(DirFromUE(v))); }
inline Rotator LookAt(Vec3 from, Vec3 to) {
    Vec3 d = to - from;
    if (kUnrealSpace) return Rotator{Degrees(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y))), Degrees(std::atan2(d.y, d.x)), 0};
    float len = d.Length();
    return len < 1e-6f ? Rotator{} : Rotator{Degrees(std::asin(Clamp(d.y / len, -1.0f, 1.0f))), Degrees(std::atan2(-d.x, -d.z)), 0};
}
inline Vec3 CrossUE(Vec3 a, Vec3 b) { return Vec3::Cross(a, b); }  // same formula as FVector::CrossProduct
inline Vec3 Mul(Vec3 a, Vec3 b) { return Vec3(a.x * b.x, a.y * b.y, a.z * b.z); }

// ---- conversions (Unreal's automatic pin conversions; anything else becomes a default value)
template <typename T> std::string ToString(const T& v);
inline std::string ToString(const std::string& s) { return s; }
inline std::string ToString(const char* s) { return s ? s : ""; }
inline std::string ToString(bool b) { return b ? "true" : "false"; }
inline std::string ToString(Vec3 v) { return std::format("X={:.3f} Y={:.3f} Z={:.3f}", v.x, v.y, v.z); }
inline std::string ToString(Rotator r) { return std::format("P={:.6f} Y={:.6f} R={:.6f}", r.pitch, r.yaw, r.roll); }
inline std::string ToString(const Transform& t) { return ToString(t.location) + "|" + ToString(t.rotation) + "|" + ToString(t.scale); }
inline std::string ToString(Color c) { return std::format("(R={:.6f},G={:.6f},B={:.6f},A=1.000000)", c.r, c.g, c.b); }
inline std::string ToString(const Entity& e) { return e.Name(); }
inline std::string ToString(const Opaque&) { return std::string(); }
template <typename T> std::string ToString(const T& v) {
    if constexpr (std::is_floating_point_v<T>) {
        std::string s = std::format("{}", static_cast<float>(v));
        if (s.find_first_of(".eEni") == std::string::npos) s += ".0";
        return s;
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(v);
    } else {
        return std::string();
    }
}
template <typename T, typename S> T As(const S& v) {
    if constexpr (std::is_same_v<T, S>) return v;
    else if constexpr (std::is_same_v<T, std::string>) return ToString(v);
    else if constexpr (std::is_same_v<T, bool> && std::is_same_v<S, std::string>) return v == "true" || v == "True" || v == "1";
    else if constexpr (std::is_arithmetic_v<T> && std::is_same_v<S, std::string>) return static_cast<T>(std::strtod(v.c_str(), nullptr));
    else if constexpr (std::is_same_v<T, bool> && std::is_same_v<S, Entity>) return v.IsValid();
    else if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<S>) return static_cast<T>(v);
    else if constexpr (std::is_same_v<T, Vec3> && std::is_arithmetic_v<S>) return Vec3(static_cast<float>(v), static_cast<float>(v), static_cast<float>(v));
    else if constexpr (std::is_same_v<T, Entity> && std::is_same_v<S, Entity>) return v;
    else return T{};
}
inline bool IsValid(const Entity& e) { return e.IsValid(); }
inline bool IsValid(const std::string& s) { return !s.empty(); }
template <typename T> bool IsValid(const T&) { return true; }
inline std::string AssetName(const std::string& path) {  // "/Game/Audio/S_Hit.S_Hit" -> "S_Hit"
    std::string s = path;
    if (size_t q = s.find('\''); q != std::string::npos) s = s.substr(q + 1);
    while (!s.empty() && (s.back() == '\'' || s.back() == '"')) s.pop_back();
    if (size_t d = s.find_last_of("./:"); d != std::string::npos) s = s.substr(d + 1);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0) s.resize(s.size() - 2);
    return s;
}
inline std::string ClassTag(const std::string& cls) { return AssetName(cls); }
inline std::string PrefabFor(const std::string& cls) { return "Prefabs/" + AssetName(cls) + ".fscene"; }
inline std::string AssetPath(const std::string& sound) { return sound.empty() ? std::string("builtin:beep") : "Audio/" + AssetName(sound) + ".wav"; }

// ---- math (Unreal semantics: division by zero gives 0)
inline float Div(float a, float b) { return b == 0 ? 0.0f : a / b; }
inline Vec3 Div(Vec3 a, float b) { return b == 0 ? Vec3() : a / b; }
inline int DivInt(int a, int b) { return b == 0 ? 0 : a / b; }
inline int ModInt(int a, int b) { return b == 0 ? 0 : a % b; }
inline float FMod(float a, float b) { return b == 0 ? 0.0f : std::fmod(a, b); }
inline float FClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int IClamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float FInterpTo(float cur, float target, float dt, float speed) {
    if (speed <= 0) return target;
    float d = target - cur;
    if (d * d < 1e-8f) return target;
    return cur + d * FClamp(dt * speed, 0, 1);
}
inline Vec3 VInterpTo(Vec3 cur, Vec3 target, float dt, float speed) {
    if (speed <= 0) return target;
    Vec3 d = target - cur;
    if (d.LengthSquared() < 1e-8f) return target;
    return cur + d * FClamp(dt * speed, 0, 1);
}
inline Rotator RInterpTo(Rotator cur, Rotator target, float dt, float speed) {
    return Rotator{FInterpTo(cur.pitch, target.pitch, dt, speed), FInterpTo(cur.yaw, target.yaw, dt, speed), FInterpTo(cur.roll, target.roll, dt, speed)};
}
inline float MapRange(float v, float inA, float inB, float outA, float outB, bool clamp) {
    float pct = inA == inB ? (v >= inB ? 1.0f : 0.0f) : (v - inA) / (inB - inA);
    if (clamp) pct = FClamp(pct, 0, 1);
    return outA + (outB - outA) * pct;
}
inline float RandomFloat() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }
inline float RandomFloatInRange(float lo, float hi) { return lo + (hi - lo) * RandomFloat(); }
inline int RandomIntInRange(int lo, int hi) { return hi <= lo ? lo : lo + std::rand() % (hi - lo + 1); }
template <typename T> T Select(int index, std::initializer_list<T> options) {
    return index >= 0 && static_cast<size_t>(index) < options.size() ? *(options.begin() + index) : T{};
}

// ---- strings (ASCII case folding)
inline std::string ToUpper(std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }
inline std::string ToLower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
inline bool Contains(const std::string& s, const std::string& sub, bool useCase) { return useCase ? s.find(sub) != std::string::npos : ToLower(s).find(ToLower(sub)) != std::string::npos; }
inline bool StartsWith(const std::string& s, const std::string& p) { return ToLower(s).rfind(ToLower(p), 0) == 0; }
inline bool EndsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && ToLower(s.substr(s.size() - p.size())) == ToLower(p); }
inline std::string Left(const std::string& s, int n) { return s.substr(0, static_cast<size_t>(std::max(0, n))); }
inline std::string Right(const std::string& s, int n) { size_t k = static_cast<size_t>(std::max(0, n)); return k >= s.size() ? s : s.substr(s.size() - k); }
inline std::string Mid(const std::string& s, int start, int n) { size_t a = static_cast<size_t>(std::max(0, start)); return a >= s.size() ? std::string() : s.substr(a, static_cast<size_t>(std::max(0, n))); }
inline std::string Replace(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    return s;
}
inline std::string Trim(std::string s, bool leading, bool trailing) {
    if (leading) s.erase(0, s.find_first_not_of(" \t\r\n"));
    if (trailing) { size_t e = s.find_last_not_of(" \t\r\n"); s.erase(e == std::string::npos ? 0 : e + 1); }
    return s;
}
inline int ParseInt(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }
inline float ParseFloat(const std::string& s) { return static_cast<float>(std::strtod(s.c_str(), nullptr)); }
inline std::string Join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : std::string()) + v[i];
    return out;
}
inline std::vector<std::string> Split(const std::string& s, const std::string& delim, bool cullEmpty) {
    std::vector<std::string> out;
    if (delim.empty()) { if (!s.empty() || !cullEmpty) out.push_back(s); return out; }
    size_t a = 0;
    for (;;) {
        size_t b = s.find(delim, a);
        std::string part = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!part.empty() || !cullEmpty) out.push_back(part);
        if (b == std::string::npos) break;
        a = b + delim.size();
    }
    return out;
}
inline std::string Format(std::string pattern, const std::vector<std::pair<std::string, std::string>>& args) {
    for (auto& [k, v] : args) pattern = Replace(pattern, "{" + k + "}", v);
    return pattern;
}

// ---- containers (Unreal sets are std::vector with unique items here; out-of-range reads give a default value)
template <typename T, typename U> int ArrayAdd(std::vector<T>& a, const U& v) { a.push_back(As<T>(v)); return static_cast<int>(a.size()) - 1; }
template <typename T, typename U> int ArrayAddUnique(std::vector<T>& a, const U& v) {
    T x = As<T>(v);
    for (size_t i = 0; i < a.size(); ++i) if (a[i] == x) return -1;
    a.push_back(x);
    return static_cast<int>(a.size()) - 1;
}
template <typename T, typename U> void ArrayInsert(std::vector<T>& a, const U& v, int i) { if (i >= 0 && static_cast<size_t>(i) <= a.size()) a.insert(a.begin() + i, As<T>(v)); }
template <typename T> void ArrayRemoveIndex(std::vector<T>& a, int i) { if (i >= 0 && static_cast<size_t>(i) < a.size()) a.erase(a.begin() + i); }
template <typename T, typename U> bool ArrayRemoveItem(std::vector<T>& a, const U& v) {
    T x = As<T>(v);
    size_t n = a.size();
    std::erase(a, x);
    return a.size() != n;
}
template <typename T, typename U> void ArraySet(std::vector<T>& a, int i, const U& v, bool sizeToFit) {
    if (i < 0) return;
    if (static_cast<size_t>(i) >= a.size()) { if (!sizeToFit) return; a.resize(static_cast<size_t>(i) + 1); }
    a[static_cast<size_t>(i)] = As<T>(v);
}
template <typename T> void ArrayShuffle(std::vector<T>& a) { for (size_t i = a.size(); i > 1; --i) std::swap(a[i - 1], a[static_cast<size_t>(std::rand()) % i]); }
template <typename T> bool ArrayIsValidIndex(const std::vector<T>& a, int i) { return i >= 0 && static_cast<size_t>(i) < a.size(); }
template <typename T> T ArrayGet(const std::vector<T>& a, int i) { return ArrayIsValidIndex(a, i) ? a[static_cast<size_t>(i)] : T{}; }
template <typename T> T ArrayRandom(const std::vector<T>& a) { return a.empty() ? T{} : a[static_cast<size_t>(std::rand()) % a.size()]; }
template <typename T, typename U> bool ArrayContains(const std::vector<T>& a, const U& v) { T x = As<T>(v); for (auto& e : a) if (e == x) return true; return false; }
template <typename T, typename U> int ArrayFind(const std::vector<T>& a, const U& v) { T x = As<T>(v); for (size_t i = 0; i < a.size(); ++i) if (a[i] == x) return static_cast<int>(i); return -1; }
template <typename K, typename V, typename A, typename B> void MapAdd(std::map<K, V>& m, const A& k, const B& v) { m[As<K>(k)] = As<V>(v); }
template <typename K, typename V, typename A> bool MapRemove(std::map<K, V>& m, const A& k) { return m.erase(As<K>(k)) > 0; }
template <typename K, typename V, typename A> bool MapContains(const std::map<K, V>& m, const A& k) { return m.find(As<K>(k)) != m.end(); }
template <typename K, typename V, typename A> V MapFind(const std::map<K, V>& m, const A& k) { auto it = m.find(As<K>(k)); return it == m.end() ? V{} : it->second; }
template <typename K, typename V> std::vector<K> MapKeys(const std::map<K, V>& m) { std::vector<K> o; for (auto& [k, v] : m) o.push_back(k); return o; }
template <typename K, typename V> std::vector<V> MapValues(const std::map<K, V>& m) { std::vector<V> o; for (auto& [k, v] : m) o.push_back(v); return o; }

// ---- flow-control macros (DoOnce, DoN, Gate, FlipFlop) as small state structs
struct DoOnce {
    bool closed = false;
    bool Enter() { if (closed) return false; closed = true; return true; }
    void Reset() { closed = false; }
};
struct DoN {
    int counter = 0;
    bool Enter(int n) { if (counter >= n) return false; ++counter; return true; }
    void Reset() { counter = 0; }
};
struct Gate {
    bool open = true;
};
struct FlipFlop {
    bool isA = false;
    bool Flip() { isA = !isA; return isA; }
};
} // namespace bp
)BPPRE"
