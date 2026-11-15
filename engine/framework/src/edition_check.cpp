#include "fledge/framework/edition_check.h"

#include <format>

#include "fledge/core/version.h"

namespace fledge {

EditionCheck checkProjectEdition(const Json& manifest, const FeatureRegistry& reg) {
    EditionCheck c;
    if (!manifest.is_object()) return c;
    std::vector<FeatureRequirement> req;
    bool unreadable = false;
    if (manifest.contains("requiredFeatures") && manifest["requiredFeatures"].is_array()) {
        for (const auto& e : manifest["requiredFeatures"]) {
            if (auto r = FeatureRequirement::fromJson(e)) req.push_back(*r);
            else unreadable = true;
        }
    } else if (manifest.contains("requiredFeatures")) {
        unreadable = true;
    }
    c.missing = missingFeatures(req, reg);
    c.required = minimumEdition(req, reg);
    if (unreadable) {  // a requirement we cannot read is never ignored
        MissingFeature m;
        m.required.id = "requiredFeatures";
        m.title = "Unreadable feature list";
        m.edition = Edition::Advanced;
        m.reason = "the project was saved by a newer Fledge (its requiredFeatures entry could not be read)";
        c.missing.push_back(m);
    }
    // "minEdition" is derived from requiredFeatures, but a manifest that asks for more is believed (conservative).
    if (manifest.contains("minEdition") && manifest["minEdition"].is_string()) {
        std::string me = manifest["minEdition"].get<std::string>();
        auto e = parseEdition(me);
        if (!e) {
            MissingFeature m;
            m.required.id = "minEdition";
            m.title = std::format("Edition '{}'", me);
            m.edition = Edition::Advanced;
            m.reason = std::format("the project asks for an edition this {} does not know", editionName(currentEdition()));
            c.missing.push_back(m);
        } else if (*e == Edition::Advanced) {
            // only when the feature list does not explain it (a list of provided Advanced features is trusted)
            bool listed = c.required == Edition::Advanced;
            c.required = Edition::Advanced;
            if (currentEdition() == Edition::Standard && c.missing.empty() && !listed) {
                MissingFeature m;
                m.required.id = "minEdition";
                m.title = "Fledge Advanced";
                m.edition = Edition::Advanced;
                m.reason = "the project manifest says it needs Fledge Advanced";
                c.missing.push_back(m);
            }
        }
    }
    c.ok = c.missing.empty();
    return c;
}

EditionCheck checkProjectEdition(const fs::path& projectFile, const FeatureRegistry& reg) {
    EditionCheck c;
    auto text = readFileText(projectFile);
    if (text) {
        Json j = Json::parse(*text, nullptr, false);
        if (!j.is_discarded()) c = checkProjectEdition(j, reg);
    }
    c.projectFile = projectFile;
    return c;
}

std::string EditionCheck::title() const {
    if (ok) return "This project can be opened";
    bool needsAdvanced = currentEdition() == Edition::Standard;
    for (const auto& m : missing)
        if (!(m.edition == Edition::Advanced && m.providedVersion == 0)) needsAdvanced = false;
    return needsAdvanced ? "This project needs Fledge Advanced" : std::format("This project needs a newer {}", editionName(currentEdition()));
}

std::string EditionCheck::explanation() const {
    if (ok) return {};
    std::string s = std::format("{} {} cannot edit this project. It uses:\n", editionName(currentEdition()), FLEDGE_VERSION_STRING);
    for (const auto& m : missing) s += std::format("  - {} ({})\n", m.title, m.reason);
    s += "\nTo keep the project safe, nothing was opened for editing and nothing in its folder was changed: no migration, no "
         "lock or temporary files, no save on exit.";
    if (currentEdition() == Edition::Standard)
        s += " Fledge Advanced can open it. Projects made with Fledge always open in Fledge Advanced, but a project that uses "
             "Advanced features only opens in Fledge Advanced.";
    else
        s += " Open it with the version of Fledge Advanced that saved it, or a newer one.";
    return s;
}

std::string EditionCheck::summary() const {
    if (ok) return {};
    std::string list;
    for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m.title + " (" + m.reason + ")";
    return std::format("{}: {}; nothing was changed", title(), list);
}

Json EditionCheck::toJson() const {
    Json m = Json::array();
    for (const auto& x : missing) m.push_back(x.toJson());
    return {{"ok", ok}, {"edition", editionId(currentEdition())}, {"requiredEdition", editionId(required)}, {"missing", m},
            {"title", title()}, {"explanation", explanation()}};
}

} // namespace fledge
