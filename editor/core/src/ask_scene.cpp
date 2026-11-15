#include "fledge/editor/ask_scene.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

#include "fledge/core/guid.h"
#include "fledge/editor/editor_session.h"

namespace fledge {

const std::vector<std::string>* WhyReport::section(const std::string& heading) const {
    for (auto& [h, lines] : sections)
        if (h == heading) return &lines;
    return nullptr;
}

std::string WhyReport::toText() const {
    std::string s = title + "\n";
    for (auto& [h, lines] : sections) {
        s += "\n" + h + ":\n";
        for (auto& l : lines) s += "  - " + l + "\n";
    }
    return s;
}

namespace {
std::string vec3(Vec3 v) { return std::format("({:.2f}, {:.2f}, {:.2f})", v.x, v.y, v.z); }
const char* lightName(LightType t) { return t == LightType::Directional ? "directional" : t == LightType::Point ? "point" : "spot"; }

// Does `j` mention entity `e` as a value under a key that looks like an entity reference?
bool refersTo(const Json& j, EntityId e, const std::string& key = {}) {
    if (j.is_object()) {
        for (auto& [k, v] : j.items())
            if (refersTo(v, e, toLower(k))) return true;
        return false;
    }
    if (j.is_array()) {
        for (auto& v : j)
            if (refersTo(v, e, key)) return true;
        return false;
    }
    bool refKey = key.find("target") != std::string::npos || key.find("entity") != std::string::npos || key.find("follow") != std::string::npos ||
                  key.find("parent") != std::string::npos || key.find("ref") != std::string::npos;
    return refKey && j.is_number_integer() && j.get<int64_t>() == static_cast<int64_t>(e);
}
} // namespace

WhyReport explainEntity(EditorSession& s, EntityId e) {
    WhyReport r;
    Scene& scene = s.scene();
    if (!scene.isValid(e)) {
        r.title = "Nothing selected";
        return r;
    }
    std::string name = scene.displayName(e);
    r.title = std::format("Why does \"{}\" (entity #{}) look and behave like this?", name, e);
    Transform wt = scene.worldTransform(e);

    // ---- what it is
    std::vector<std::string> what;
    std::string chain = name;
    for (EntityId p = scene.parentOf(e); p != kNullEntity; p = scene.parentOf(p)) chain = scene.displayName(p) + " > " + chain;
    what.push_back("Path in the scene: " + chain);
    what.push_back("World position " + vec3(wt.position) + ", scale " + vec3(wt.scale));
    if (auto* t = scene.get<TagsComponent>(e); t && !t->tags.empty()) what.push_back("Tags: " + t->tags);
    r.sections.push_back({"What it is", what});

    // ---- what draws it
    std::vector<std::string> draw;
    if (auto* mr = scene.get<MeshRendererComponent>(e)) {
        draw.push_back(std::format("MeshRenderer draws mesh '{}'{}", mr->mesh, mr->visible ? "" : " - but it is switched OFF (visible = false)"));
        if (!mr->material.empty()) {
            std::string path = mr->material;
            if (auto g = Guid::parse(mr->material))
                if (const AssetRecord* rec = s.registry().find(*g)) path = rec->path;
            draw.push_back("Its look comes from the material " + path);
            if (auto text = s.vfs().readText("content:/" + path)) {
                Json m = Json::parse(*text, nullptr, false);
                if (m.is_object()) {
                    std::string facts;
                    for (const char* k : {"baseColor", "roughness", "metallic", "emissive", "baseColorTexture", "normalTexture"})
                        if (m.contains(k)) facts += std::format("{}{} = {}", facts.empty() ? "" : ", ", k, m[k].dump());
                    if (!facts.empty()) draw.push_back("  material values: " + facts);
                }
            }
        } else {
            draw.push_back("No material: the legacy colour " + vec3(mr->color) + (mr->texture.empty() ? "" : " and texture " + mr->texture) + " are used");
        }
        draw.push_back("Rendered in the forward PBR opaque pass; it casts shadows into the directional cascades and receives them");
        std::vector<std::string> lit;
        for (EntityId l : scene.entities()) {
            auto* lc = scene.get<LightComponent>(l);
            if (!lc) continue;
            float d = length(scene.worldTransform(l).position - wt.position);
            if (lc->type == LightType::Directional || d <= lc->range)
                lit.push_back(std::format("{} ({} light, intensity {:.2f}{})", scene.displayName(l), lightName(lc->type), lc->intensity,
                                          lc->type == LightType::Directional ? "" : std::format(", {:.1f} m away, range {:.1f} m", d, lc->range)));
        }
        draw.push_back(lit.empty() ? "No light reaches it: only the sky / ambient light shades it" : "Lit by: " + [&] {
            std::string j;
            for (auto& x : lit) j += (j.empty() ? "" : "; ") + x;
            return j;
        }());
    }
    if (auto* lc = scene.get<LightComponent>(e)) {
        int n = 0;
        for (EntityId m : scene.entities())
            if (scene.get<MeshRendererComponent>(m) && (lc->type == LightType::Directional || length(scene.worldTransform(m).position - wt.position) <= lc->range)) ++n;
        draw.push_back(std::format("It is a {} light (colour {}, intensity {:.2f}) lighting {} mesh{}; shadows {}", lightName(lc->type), vec3(lc->color),
                                   lc->intensity, n, n == 1 ? "" : "es", lc->castShadows ? "on" : "off"));
    }
    if (auto* cam = scene.get<CameraComponent>(e))
        draw.push_back(std::format("It is a camera ({:.0f} deg FOV){}", cam->fovDegrees, cam->primary ? " - the primary one, so Play looks through it" : ""));
    if (draw.empty()) draw.push_back("Nothing: it has no MeshRenderer, Light or Camera (an empty / grouping entity)");
    r.sections.push_back({"What draws it", draw});

    // ---- what moves it
    std::vector<std::string> moves;
    if (auto* rot = scene.get<RotatorComponent>(e)) moves.push_back(std::format("Rotator spins it {:.0f} deg/s around {}", rot->degreesPerSecond, vec3(rot->axis)));
    if (auto* b = scene.get<BehavioursComponent>(e)) {
        for (auto& beh : b->list) {
            std::string cls = beh.value("class", "?");
            std::string line = "Fledge Script " + cls + (beh.value("enabled", true) ? "" : " (disabled)");
            if (beh.contains("properties") && beh["properties"].is_object() && !beh["properties"].empty()) line += " with " + beh["properties"].dump();
            if (s.hasProject()) {
                fs::path src = s.project().scriptsDir() / pathFromUtf8(cls + ".cpp");
                std::error_code ec;
                if (fs::exists(src, ec)) line += " - code in Scripts/" + cls + ".cpp";
            }
            moves.push_back(line);
        }
    }
    if (auto* sc = scene.get<ScriptComponent>(e)) moves.push_back("Gameplay module " + sc->module + " behaviour " + sc->behaviour);
    if (moves.empty()) moves.push_back("Nothing moves it at runtime (no Rotator, scripts or modules): it stays where it was placed");
    r.sections.push_back({"What moves it", moves});

    // ---- last changes (undo history)
    std::vector<std::string> changes;
    const auto& cmds = s.history().commands();
    for (auto it = cmds.rbegin(); it != cmds.rend() && changes.size() < 6; ++it)
        if (it->before.count(e) || it->after.count(e))
            changes.push_back(std::format("\"{}\" by {} ({:.0f} s after the session started)", it->label, it->source == EditSource::AI ? "the AI" : toString(it->source),
                                          it->timeSeconds));
    if (changes.empty()) changes.push_back("Not changed in this session (it comes from the saved scene)");
    r.sections.push_back({"Last changes", changes});

    // ---- if deleted
    std::vector<std::string> del;
    std::function<int(EntityId)> count = [&](EntityId x) {
        int n = 0;
        for (EntityId c : scene.childrenOf(x)) n += 1 + count(c);
        return n;
    };
    int kids = count(e);
    if (kids) del.push_back(std::format("{} child entit{} would be deleted with it", kids, kids == 1 ? "y" : "ies"));
    for (EntityId o : scene.entities()) {
        if (o == e) continue;
        Json oj = scene.entityToJson(o);
        if (oj.contains("components") && refersTo(oj["components"], e))
            del.push_back(scene.displayName(o) + " refers to it (a target/entity field) and would lose its reference");
    }
    if (s.hasProject()) {
        std::error_code ec;
        std::string needle = "\"" + name + "\"";
        for (auto it = fs::recursive_directory_iterator(s.project().scriptsDir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || (it->path().extension() != ".cpp" && it->path().extension() != ".h")) continue;
            auto text = readFileText(it->path());
            if (text && text->find(needle) != std::string::npos)
                del.push_back("Scripts/" + narrow(it->path().lexically_relative(s.project().scriptsDir()).generic_wstring()) + " looks it up by name " + needle);
        }
    }
    if (del.empty()) del.push_back("Nothing else depends on it");
    r.sections.push_back({"If you delete it", del});
    return r;
}

} // namespace fledge
