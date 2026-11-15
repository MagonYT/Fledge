// Content Browser (thumbnails, drag-and-drop, import/cook/re-import), Material
// Editor panel and asset instantiation from drops.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_set>

#include "fledge/assets/cooked.h"
#include "fledge/importers/archive.h"
#include "fledge/importers/importers.h"
#include "editor_app.h"

namespace fledge::editor {

namespace {
constexpr int kThumbSize = 72;

const char* typeBadge(AssetType t) {
    switch (t) {
        case AssetType::Mesh: return "MESH";
        case AssetType::Texture: return "TEX";
        case AssetType::Material: return "MAT";
        case AssetType::Model: return "MODEL";
        case AssetType::Scene: return "SCENE";
        case AssetType::Audio: return "AUDIO";
        default: return "FILE";
    }
}
} // namespace

void EditorApp::releaseThumbnails() {
    for (auto& [k, t] : m_thumbs) {
        if (t.rt) m_device->destroyRenderTarget(t.rt);
        if (t.tex) m_device->destroyTexture(t.tex);
    }
    m_thumbs.clear();
}

ImTextureID EditorApp::thumbnail(const AssetRecord& rec) {
    if (auto it = m_thumbs.find(rec.path); it != m_thumbs.end()) return static_cast<ImTextureID>(it->second.native);
    if (m_thumbBudget <= 0) return 0;  // spread thumbnail generation over frames
    --m_thumbBudget;
    Thumb t;
    if (rec.type == AssetType::Texture) {
        auto bytes = m_session.vfs().read("content:/" + rec.path);
        std::optional<TextureData> tex;
        if (bytes) {
            if (endsWith(toLower(rec.path), ".ctex")) {
                if (auto c = readCookedTexture(*bytes)) tex = decompressToRgba8(*c);
            } else if (auto img = decodeImage(*bytes)) {
                tex = uncompressedTexture(*img, true);
            }
        }
        if (tex && !tex->mips.empty()) {
            const TextureMip* best = &tex->mips.back();
            for (auto& m : tex->mips)
                if (m.width <= 128 && m.height <= 128) {
                    best = &m;
                    break;
                }
            t.tex = m_device->createTexture2D(best->width, best->height, best->data.data());
        }
    } else if (rec.type == AssetType::Mesh || rec.type == AssetType::Material || rec.type == AssetType::Model) {
        std::string mesh = "builtin:sphere", material;
        std::string guid = rec.guid.toString();
        if (rec.type == AssetType::Mesh) mesh = guid;
        else if (rec.type == AssetType::Material) material = guid;
        else if (auto text = m_session.vfs().readText("content:/" + rec.path)) {
            auto model = ModelAsset::fromJson(Json::parse(*text, nullptr, false));
            if (model)
                for (auto& n : model->nodes)
                    if (!n.meshes.empty()) {
                        mesh = n.meshes[0].mesh;
                        material = n.meshes[0].material;
                        break;
                    }
        }
        t.rt = m_device->createRenderTarget(kThumbSize, kThumbSize);
        m_session.world().renderAssetPreview(t.rt, kThumbSize, mesh, material);
    }
    t.native = t.rt ? m_device->nativeColorTexture(t.rt) : t.tex ? m_device->nativeTexture(t.tex) : 0;
    t.flipY = t.rt && m_device->backend() == rhi::Backend::OpenGL;  // GL textures are bottom-up; D3D11 is top-down
    m_thumbs[rec.path] = t;
    return static_cast<ImTextureID>(t.native);
}

void EditorApp::dropAsset(const std::string& rel, EntityId target, std::optional<Vec3> worldPos) {
    const AssetRecord* rec = m_session.registry().findByPath(rel);
    if (!rec) {
        m_status = "not a registered asset: " + rel;
        return;
    }
    std::string guid = rec->guid.toString();
    switch (rec->type) {
        case AssetType::Material:
            if (target) {
                auto st = m_session.assignMaterial(target, guid);
                m_status = st ? "Material assigned" : st.error;
            } else {
                m_status = "Drop a material onto an entity to assign it";
            }
            break;
        case AssetType::Texture:
            if (target) {
                auto st = m_session.setProperty(target, "MeshRenderer", "texture", guid);
                m_status = st ? "Texture assigned (legacy MeshRenderer.texture)" : st.error;
            }
            break;
        case AssetType::Mesh:
        case AssetType::Model: {
            // Dropped in the viewport: place at the hit point (root). Dropped on a hierarchy row: child of that row.
            EntityId parent = worldPos ? kNullEntity : target;
            auto e = m_session.instantiateAsset(guid, parent, worldPos.value_or(Vec3{0, 0, 0}));
            if (e) m_session.select(*e);
            m_status = e ? "Instantiated " + rel : e.error();
            break;
        }
        case AssetType::Scene: {
            auto st = m_session.loadScene("content:/" + rel);
            m_status = st ? "Loaded " + rel : st.error;
            break;
        }
        case AssetType::Audio:
            if (target) {  // dropped on an entity: it gets (or updates) an AudioSource with this clip
                auto st = m_session.applyComponents(target, {{"AudioSource", {{"clip", guid}}}}, "Assign sound");
                m_status = st ? "Sound assigned (AudioSource)" : st.error;
            } else {
                m_status = m_session.world().previewClip(guid) ? "Previewing " + rel : "cannot play " + rel;
            }
            break;
        default: break;
    }
}

Result<int> EditorApp::startAssetImport(const fs::path& source, const ModelImportOptions& options, bool instantiate) {
    if (!m_session.hasProject()) return Result<int>::failure(kNoProjectHelp);
    if (std::count_if(m_assetImportJobs.begin(), m_assetImportJobs.end(), [](const auto& job) {
            return job.second["state"] == "importing";
        }) >= 4) return Result<int>::failure("Four asset imports are already running. Check their status and retry.");
    if (m_assetImportJobs.size() >= 128) {
        auto finished = std::find_if(m_assetImportJobs.begin(), m_assetImportJobs.end(), [](const auto& job) {
            return job.second["state"] != "importing";
        });
        if (finished != m_assetImportJobs.end()) m_assetImportJobs.erase(finished);
    }

    const Project project = m_session.project();
    const auto stage = project.rootDir() / "Intermediate" / "AssetImport" / pathFromUtf8(Guid::generate().toString());
    if (!archive::pathWithin(project.rootDir(), stage))
        return Result<int>::failure("The import staging folder must be inside the project.");
    const int id = m_nextAssetImportId++;
    m_assetImportJobs.emplace(id, Json{{"id", id}, {"state", "importing"}});
    runInBackground("Importing " + narrow(source.filename().wstring()), [this, id, stage, source, options, project, instantiate](BackgroundTask& task) {
        task.progress = -1;
        auto staging = std::make_shared<DirectoryProvider>(stage);
        Vfs vfs;
        vfs.mount("content", staging);
        AssetRegistry registry;
        auto imported = [&]() -> Result<ImportResult> {
            try {
                return fledge::importAsset(source, vfs, registry, options);
            } catch (const std::exception& error) {
                return Result<ImportResult>::failure(error.what());
            }
        }();
        auto files = staging->list();
        std::erase(files, AssetRegistry::kFileName);
        m_mainQueue.post([this, id, stage, project, imported = std::move(imported), files = std::move(files), instantiate]() {
            auto& job = m_assetImportJobs.at(id);
            DirectoryProvider staged(stage);
            DirectoryProvider content(project.contentDir());
            std::vector<std::pair<fs::path, fs::path>> moves;
            std::error_code ec;
            std::string failure;
            if (!imported) failure = imported.error();
            else if (!m_session.hasProject() || m_session.project().file != project.file) failure = "The project changed before the import finished.";
            else if (std::find(files.begin(), files.end(), imported->primary.path) == files.end()) failure = "The importer did not produce its primary asset.";
            if (failure.empty()) {
                for (const auto& rel : files) {
                    auto to = content.resolve(rel);
                    if (to.empty() || !archive::pathWithin(content.root(), to) || fs::exists(osPath(to), ec) || ec) {
                        failure = "Background import cannot publish over an existing or inaccessible file: " + rel;
                        break;
                    }
                    moves.emplace_back(staged.resolve(rel), std::move(to));
                }
            }
            size_t moved = 0;
            if (failure.empty()) {
                for (const auto& [from, to] : moves) {
                    fs::create_directories(osPath(to.parent_path()), ec);
                    if (!ec && !fs::exists(osPath(to), ec) && !ec) fs::rename(osPath(from), osPath(to), ec);
                    else if (!ec) ec = std::make_error_code(std::errc::file_exists);
                    if (ec) {
                        failure = "Could not publish " + narrow(to.filename().wstring()) + ": " + ec.message();
                        break;
                    }
                    ++moved;
                }
            }
            if (failure.empty()) {
                AssetRegistry updated = m_session.registry();
                for (const auto& asset : imported->created)
                    if (asset.guid.isValid()) updated.addWithGuid(asset.path, asset.type, asset.guid);
                if (!updated.save(m_session.vfs())) failure = "Could not save the updated asset registry.";
                else m_session.registry() = std::move(updated);
            }
            if (!failure.empty()) {
                for (size_t i = 0; i < moved; ++i) fs::remove(osPath(moves[i].second), ec);
                job["state"] = "failed";
                job["error"] = failure;
                toast(ToastLevel::Error, failure);
            } else {
                std::vector<std::string> changed;
                for (const auto& asset : imported->created) changed.push_back(asset.path);
                m_session.invalidateChangedAssets(changed);
                job["state"] = "imported";
                job["primary"] = {{"guid", imported->primary.guid.toString()}, {"path", "content:/" + imported->primary.path}};
                job["warnings"] = imported->warnings;
                job["created"] = Json::array();
                for (const auto& asset : imported->created)
                    job["created"].push_back({{"guid", asset.guid.toString()}, {"path", "content:/" + asset.path}, {"type", toString(asset.type)}});
                if (instantiate) {
                    auto entity = m_session.instantiateAsset(imported->primary.guid.toString());
                    if (entity) job["entity"] = *entity;
                    else job["instantiateError"] = entity.error();
                }
                toast(ToastLevel::Success, "Imported " + imported->primary.path);
            }
            if (archive::pathWithin(project.rootDir(), stage)) fs::remove_all(osPath(stage), ec);
        });
        return std::string{};
    });
    return id;
}

void EditorApp::drawContentBrowser() {
    if (!ImGui::Begin("Content Browser", &panel("Content Browser")->open)) {
        ImGui::End();
        return;
    }
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }
    if (m_thumbGeneration != m_session.assetGeneration()) {  // assets changed on disk / were edited
        releaseThumbnails();
        m_thumbGeneration = m_session.assetGeneration();
    }
    m_thumbBudget = 2;

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::InputTextWithHint("##import", "path to .gltf / .glb / .obj / .png to import", &m_importPath);
    ImGui::SameLine();
    if (ImGui::Button("Import") && !m_importPath.empty()) {
        std::string p = m_importPath;
        std::erase(p, '"');
        auto r = m_session.importAsset(pathFromUtf8(p));
        m_status = r ? std::format("Imported {} ({} assets, {} warnings)", r->primary.path, r->created.size(), r->warnings.size())
                     : "Import failed: " + r.error();
        if (r) m_importPath.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_cookRunning);
    if (ImGui::Button(m_cookRunning ? "Cooking..." : "Cook")) startCook(false);  // background task (status bar shows progress)
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ Material")) ImGui::OpenPopup("NewMaterial");
    if (ImGui::BeginPopup("NewMaterial")) {
        ImGui::InputText("name", &m_newMaterialName);
        if (ImGui::Button("Create")) {
            auto r = m_session.createMaterial(m_newMaterialName);
            m_status = r ? "Created " + r->path : r.error();
            if (r) openMaterialEditor(r->guid.toString());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##filter", "filter", &m_assetFilter);
    ImGui::SameLine();
    ImGui::TextDisabled("watch: %s", m_session.watcherBackend());
    ImGui::SameLine();
    if (ImGui::Checkbox("Scripts", &m_cbShowScripts)) {}
    ImGui::SameLine();
    if (ImGui::Checkbox("Used in this scene", &m_cbUsedOnly)) {}
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show only assets this scene assigns: meshes, materials, textures, characters");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the project's Fledge Script files (Scripts/)");
    ImGui::Separator();
    if (m_cbShowScripts) {  // Fledge Script files live outside Content/: list them here too
        std::error_code ec;
        fs::path dir = m_session.project().scriptsDir();
        ImGui::BeginChild("scripts");
        float avail = ImGui::GetContentRegionAvail().x;
        int columns = std::max(1, static_cast<int>(avail / (kThumbSize + 24)));
        int col = 0;
        std::vector<fs::path> files;
        if (fs::is_directory(dir, ec))
            for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                auto ext = it->path().extension();
                if (it->is_regular_file() && (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".inl")) files.push_back(it->path());
            }
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            std::string rel = narrow(f.lexically_relative(dir).generic_wstring());
            if (!m_assetFilter.empty() && toLower(rel).find(toLower(m_assetFilter)) == std::string::npos) continue;
            ImGui::PushID(rel.c_str());
            ImGui::BeginGroup();
            ImGui::PushFont(m_fontLarge);
            ImGui::Button(f.extension() == ".cpp" ? "\xef\x84\xa1" : "\xef\x85\x9b", {static_cast<float>(kThumbSize), static_cast<float>(kThumbSize)});
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scripts/%s\ndouble-click: open in the Script Editor", rel.c_str());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openScriptFile(rel);
            }
            if (ImGui::BeginPopupContextItem("sctx")) {
                if (ImGui::MenuItem("Open in Script Editor")) openScriptFile(rel);
                if (ImGui::MenuItem("Open in external editor")) openInExternalIde(rel, 1);
                ImGui::EndPopup();
            }
            std::string label = narrow(f.filename().wstring());
            if (label.size() > 12) label = label.substr(0, 11) + "~";
            ImGui::TextDisabled("SCRIPT");
            ImGui::Text("%s", label.c_str());
            ImGui::EndGroup();
            ImGui::PopID();
            if (++col < columns) ImGui::SameLine();
            else col = 0;
        }
        if (files.empty()) ImGui::TextDisabled("No scripts yet (Scripts > New Script...)");
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    ImGui::BeginChild("assets");
    float avail = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>(avail / (kThumbSize + 24)));
    int col = 0;
    std::string filter = toLower(m_assetFilter);
    if (m_cbUsedOnly) {
        size_t stamp = m_session.scene().entities().size() ^ (m_session.history().commands().size() << 1);
        if (m_session.playing()) stamp += static_cast<size_t>(m_frame / 60);
        if (stamp != m_sceneUsedStamp) {
            m_sceneUsedStamp = stamp;
            m_sceneUsed.clear();
            auto add = [&](const std::string& s) {
                if (!s.empty() && !s.starts_with("builtin:")) m_sceneUsed.insert(s);
            };
            m_session.scene().each<MeshRendererComponent, TransformComponent>([&](EntityId, MeshRendererComponent& mr, TransformComponent&) {
                add(mr.mesh);
                add(mr.texture);
                add(mr.material);
                if (auto mat = m_session.loadMaterial(mr.material)) {
                    add(mat->baseColorTexture);
                    add(mat->normalTexture);
                    add(mat->metallicRoughnessTexture);
                    add(mat->occlusionTexture);
                    add(mat->emissiveTexture);
                }
            });
            m_session.scene().each<AnimatorComponent, TransformComponent>([&](EntityId, AnimatorComponent& ac, TransformComponent&) {
                add(ac.character);
                add(ac.material);
            });
        }
    }
    for (const auto& rec : m_session.registry().all()) {
        if (!filter.empty() && toLower(rec.path).find(filter) == std::string::npos) continue;
        if (m_cbUsedOnly && rec.type != AssetType::Scene && !m_sceneUsed.contains(rec.path) && !m_sceneUsed.contains(rec.guid.toString())) continue;
        ImGui::PushID(rec.path.c_str());
        ImGui::BeginGroup();
        ImTextureID tex = thumbnail(rec);
        auto th = m_thumbs.find(rec.path);
        bool flip = th != m_thumbs.end() && th->second.flipY;
        ImVec2 size{static_cast<float>(kThumbSize), static_cast<float>(kThumbSize)};
        if (tex) ImGui::ImageButton("thumb", tex, size, {0, flip ? 1.0f : 0.0f}, {1, flip ? 0.0f : 1.0f});
        else ImGui::Button(typeBadge(rec.type), size);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kAssetPayload, rec.path.c_str(), rec.path.size() + 1);
            ImGui::Text("%s %s", typeBadge(rec.type), rec.path.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s  GUID %s\ndouble-click: %s", rec.path.c_str(), toString(rec.type), rec.guid.toString().c_str(),
                              rec.type == AssetType::Material ? "edit" : rec.type == AssetType::Scene ? "open" : rec.type == AssetType::Audio ? "preview" : "instantiate");
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (rec.type == AssetType::Material) openMaterialEditor(rec.guid.toString());
                else if (rec.type == AssetType::Audio) dropAsset(rec.path, kNullEntity, std::nullopt);
                else dropAsset(rec.path, kNullEntity, Vec3{0, 0, 0});
            }
        }
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem("Instantiate", nullptr, false, rec.type == AssetType::Mesh || rec.type == AssetType::Model))
                dropAsset(rec.path, kNullEntity, Vec3{0, 0, 0});
            if (ImGui::MenuItem("Assign to selection", nullptr, false, rec.type == AssetType::Material && !m_session.selection.empty()))
                for (EntityId e : m_session.selection) (void)m_session.assignMaterial(e, rec.guid.toString());
            if (ImGui::MenuItem("Edit material", nullptr, false, rec.type == AssetType::Material)) openMaterialEditor(rec.guid.toString());
            if (ImGui::MenuItem("Re-import")) {
                auto r = m_session.reimportAsset(rec.guid.toString());
                m_status = r ? "Re-imported " + rec.path : r.error();
            }
            if (ImGui::MenuItem("Copy GUID")) ImGui::SetClipboardText(rec.guid.toString().c_str());
            ImGui::EndPopup();
        }
        std::string label = narrow(pathFromUtf8(rec.path).filename().wstring());
        if (label.size() > 12) label = label.substr(0, 11) + "~";
        ImGui::TextDisabled("%s", typeBadge(rec.type));
        ImGui::Text("%s", label.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        if (++col < columns) ImGui::SameLine();
        else col = 0;
    }
    ImGui::EndChild();
    ImGui::End();
}

namespace {
bool editWidget(gameplay::Widget& widget, bool root) {
    using namespace gameplay;
    ImGui::PushID(&widget);
    const std::string label = std::string(toString(widget.type)) + (widget.id.empty() ? "" : "  " + widget.id);
    bool remove = false;
    if (ImGui::TreeNodeEx("widget", root ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", label.c_str())) {
        ImGui::InputText("ID", &widget.id);
        ImGui::Checkbox("Visible", &widget.visible);
        ImGui::SameLine();
        ImGui::Checkbox("Enabled", &widget.enabled);
        const char* anchors[] = {"Top left", "Top", "Top right", "Left", "Center", "Right", "Bottom left", "Bottom", "Bottom right"};
        int anchor = static_cast<int>(widget.anchor);
        if (ImGui::Combo("Anchor", &anchor, anchors, IM_ARRAYSIZE(anchors))) widget.anchor = static_cast<Anchor>(anchor);
        ImGui::DragFloat2("Offset", &widget.offset.x, 1.0f);
        ImGui::DragFloat2("Size", &widget.size.x, 1.0f, 0.0f, 4096.0f);
        if (widget.type == WidgetType::Text || widget.type == WidgetType::Button) {
            ImGui::InputText("Text", &widget.text);
            ImGui::DragFloat("Text size", &widget.textSize, 0.1f, 0.1f, 20.0f);
        }
        if (widget.type == WidgetType::Bar || widget.type == WidgetType::Slider) ImGui::SliderFloat("Fill", &widget.value, 0.0f, 1.0f);
        ImGui::DragFloat("Opacity", &widget.opacity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Scale", &widget.scale, 0.01f, 0.1f, 4.0f);
        ImGui::DragFloat("Rotation", &widget.rotation, 0.5f, -180.0f, 180.0f);
        ImGui::DragFloat("Corner radius", &widget.radius, 0.5f, 0.0f, 128.0f);
        if (ImGui::TreeNode("Animations")) {
            for (size_t i = 0; i < widget.animations.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                auto& anim = widget.animations[i];
                ImGui::InputText("Property", &anim.property);
                ImGui::DragFloat("From", &anim.from, 0.01f);
                ImGui::DragFloat("To", &anim.to, 0.01f);
                ImGui::DragFloat("Duration", &anim.duration, 0.01f, 0.0f, 8.0f);
                ImGui::InputText("Easing", &anim.easing);
                ImGui::InputText("Play", &anim.play);
                ImGui::Checkbox("Ping pong", &anim.pingPong);
                if (ImGui::Button("Remove animation")) {
                    widget.animations.erase(widget.animations.begin() + static_cast<std::ptrdiff_t>(i));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Add fade in"))
                widget.animations.push_back({"opacity", 0, 1, 0.25f, 0, "easeOut", "show", false, 0});
            ImGui::SameLine();
            if (ImGui::Button("Add hover scale"))
                widget.animations.push_back({"scale", 1, 1.05f, 0.12f, 0, "easeOut", "hover", false, 0});
            ImGui::TreePop();
        }
        if (ImGui::ColorEdit4("Color", &widget.color.x)) widget.customColor = true;
        if (ImGui::ColorEdit4("Background", &widget.background.x)) widget.customBackground = true;
        if (ImGui::Button("Use preset colors")) {
            widget.customColor = widget.customBackground = false;
            widget.color = {1, 1, 1, 1};
            widget.background = {};
        }
        if (widget.type == WidgetType::Panel || widget.type == WidgetType::Stack) {
            ImGui::DragFloat("Padding", &widget.padding, 1.0f, 0.0f, 256.0f);
            if (widget.type == WidgetType::Stack) {
                ImGui::Checkbox("Horizontal", &widget.horizontal);
                ImGui::DragFloat("Spacing", &widget.spacing, 1.0f, 0.0f, 256.0f);
            }
            for (auto child = widget.children.begin(); child != widget.children.end();) {
                if (editWidget(*child, false)) child = widget.children.erase(child);
                else ++child;
            }
            if (ImGui::Button("Add widget")) ImGui::OpenPopup("widget-types");
            if (ImGui::BeginPopup("widget-types")) {
                for (auto type : {WidgetType::Panel, WidgetType::Stack, WidgetType::Text, WidgetType::Button, WidgetType::Bar,
                                  WidgetType::Slider, WidgetType::Spacer}) {
                    if (ImGui::MenuItem(toString(type))) {
                        Widget child;
                        child.type = type;
                        if (type == WidgetType::Text || type == WidgetType::Button) child.text = toString(type);
                        if (type == WidgetType::Bar || type == WidgetType::Slider || type == WidgetType::Spacer) child.size = {160, 16};
                        widget.children.push_back(std::move(child));
                    }
                }
                ImGui::EndPopup();
            }
        }
        if (!root) remove = ImGui::Button("Remove widget");
        ImGui::TreePop();
    }
    ImGui::PopID();
    return remove;
}
} // namespace

void EditorApp::drawUiEditor() {
    using namespace gameplay;
    ImGui::SetNextWindowSize({920, 560}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UI Editor", &panel("UI Editor")->open)) {
        ImGui::End();
        return;
    }
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project to edit a UI document.");
        ImGui::End();
        return;
    }
    if (m_uiEditorProject != m_session.project().file) {
        m_uiEditorProject = m_session.project().file;
        m_uiEditorDocument.reset();
        m_uiEditorStatus.clear();
    }
    ImGui::InputText("Content path", &m_uiEditorPath);
    const std::string path = "content:/" + m_uiEditorPath;
    auto* directory = dynamic_cast<DirectoryProvider*>(m_session.vfs().provider("content"));
    const fs::path relativePath = pathFromUtf8(m_uiEditorPath);
    const fs::path destination = directory ? directory->resolve(m_uiEditorPath) : fs::path{};
    const bool validPath = !destination.empty() && !relativePath.has_root_path() &&
                           endsWith(toLower(m_uiEditorPath), ".fui") &&
                           archive::pathWithin(directory->root(), destination);
    ImGui::BeginDisabled(!validPath);
    if (ImGui::Button("Load")) {
        auto contents = m_session.vfs().readText(path);
        auto doc = contents ? UiDocument::fromJson(Json::parse(*contents, nullptr, false))
                            : Result<UiDocument>::failure("Document not found.");
        if (doc) {
            m_uiEditorDocument = std::move(*doc);
            m_uiEditorBaseline = std::move(contents);
            m_uiEditorLoadedPath = path;
            m_uiEditorStatus = "Loaded";
        } else m_uiEditorStatus = doc.error();
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        if (m_session.vfs().exists(path)) m_uiEditorStatus = "That document already exists. Use Load.";
        else {
            m_uiEditorDocument.emplace();
            m_uiEditorDocument->setStyle("FledgeGame");
            auto& root = m_uiEditorDocument->root();
            root.type = WidgetType::Stack;
            root.anchor = Anchor::Center;
            root.padding = 16;
            m_uiEditorBaseline.reset();
            m_uiEditorLoadedPath = path;
            m_uiEditorStatus = "New document";
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_uiEditorDocument || path != m_uiEditorLoadedPath);
    if (ImGui::Button("Save")) {
        auto json = m_uiEditorDocument->toJson();
        if (m_session.vfs().readText(path) != m_uiEditorBaseline)
            m_uiEditorStatus = "The file changed on disk. Load it again before saving.";
        else if (auto valid = UiDocument::fromJson(json); !valid)
            m_uiEditorStatus = valid.error();
        else {
            std::string contents = json.dump(2);
            if (m_session.vfs().writeText(path, contents)) {
                m_uiEditorBaseline = std::move(contents);
                m_session.world().invalidateAsset(path);
                m_uiEditorStatus = "Saved. Active UI screens reload on the next frame.";
            } else m_uiEditorStatus = "Could not save the document.";
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::TextUnformatted(m_uiEditorStatus.c_str());
    if (m_uiEditorDocument) {
        auto& doc = *m_uiEditorDocument;
        ImGui::BeginChild("Widget tree", {300, 0}, ImGuiChildFlags_Borders);
        const char* styles[] = {"", "FledgeDev", "FledgeGame", "FledgeLight"};
        int style = 0;
        for (int i = 1; i < IM_ARRAYSIZE(styles); ++i) if (doc.style() == styles[i]) style = i;
        if (ImGui::Combo("Style", &style, "Default\0FledgeDev\0FledgeGame\0FledgeLight\0")) doc.setStyle(styles[style]);
        editWidget(doc.root(), true);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Preview", {0, 0}, ImGuiChildFlags_Borders);
        ImGui::Combo("Aspect ratio", &m_uiEditorAspect, "16:9\0" "4:3\0" "9:16\0");
        constexpr Vec2 sizes[] = {{1280, 720}, {960, 720}, {720, 1280}};
        const Vec2 size = sizes[m_uiEditorAspect];
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float scale = std::min(avail.x / size.x, avail.y / size.y);
        if (scale > 0) {
            UiCanvas canvas(size.x, size.y);
            if (const auto* font = m_session.world().uiFont()) canvas.setFont(font);
            doc.layout(canvas);
            doc.tick(ImGui::GetIO().DeltaTime);
            doc.draw(canvas);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 corner{origin.x + size.x * scale, origin.y + size.y * scale};
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, corner, IM_COL32(23, 26, 34, 255));
            draw->PushClipRect(origin, corner, true);
            const auto& vertices = canvas.vertices();
            for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
                auto pos = [&](size_t n) { return ImVec2{origin.x + vertices[n].pos.x * scale, origin.y + vertices[n].pos.y * scale}; };
                const Vec4 color = vertices[i].color;
                draw->AddTriangleFilled(pos(i), pos(i + 1), pos(i + 2), ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, color.w}));
            }
            draw->PopClipRect();
            ImGui::Dummy({size.x * scale, size.y * scale});
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::openMaterialEditor(const std::string& ref) {
    if (PanelDef* p = panel("Material Editor")) p->open = true;
    m_materialEditRef = ref;
    m_focusMaterialEditor = true;
}

void EditorApp::drawMaterialEditor() {
    if (m_focusMaterialEditor) {
        ImGui::SetNextWindowFocus();
        m_focusMaterialEditor = false;
    }
    if (!ImGui::Begin("Material Editor", &panel("Material Editor")->open)) {
        ImGui::End();
        return;
    }
    auto mat = m_materialEditRef.empty() ? std::nullopt : m_session.loadMaterial(m_materialEditRef);
    if (!mat) {
        ImGui::TextDisabled("Double-click a material in the Content Browser, or use 'Edit' next to MeshRenderer.material.");
        ImGui::End();
        return;
    }
    std::string path = m_session.registry().resolve(m_materialEditRef);
    ImGui::Text("%s", path.c_str());
    if (const AssetRecord* rec = m_session.registry().findByPath(path.substr(std::min<size_t>(9, path.size())))) {
        if (ImTextureID tex = thumbnail(*rec)) {
            bool flip = m_device->backend() == rhi::Backend::OpenGL;
            ImGui::Image(tex, {96, 96}, {0, flip ? 1.0f : 0.0f}, {1, flip ? 0.0f : 1.0f});
        }
        if (!m_session.selection.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Assign to selection"))
                for (EntityId e : m_session.selection) (void)m_session.assignMaterial(e, rec->guid.toString());
        }
    }
    ImGui::Separator();
    const TypeInfo& ti = Material::typeInfo();
    auto commit = [&](const PropertyInfo& p, const Json& v) {
        auto st = m_session.setMaterialProperty(m_materialEditRef, p.name, v, "mat:" + m_materialEditRef + ":" + p.name);
        if (!st) m_status = st.error;
    };
    Material& m = *mat;
    for (const auto& p : ti.properties) {
        ImGui::PushID(p.name.c_str());
        PropertyValue v = p.get(&m);
        const char* label = p.name.c_str();
        switch (p.type) {
            case PropertyType::Bool: {
                bool b = std::get<bool>(v);
                if (ImGui::Checkbox(label, &b)) commit(p, b);
                break;
            }
            case PropertyType::Float: {
                float f = std::get<float>(v);
                if (ImGui::SliderFloat(label, &f, p.minValue, p.maxValue)) commit(p, f);
                break;
            }
            case PropertyType::Color: {
                Vec3 c = std::get<Vec3>(v);
                if (ImGui::ColorEdit3(label, &c.x)) commit(p, Json(c));
                break;
            }
            case PropertyType::Enum: {
                int i = std::get<int>(v);
                if (ImGui::BeginCombo(label, p.enumNames[static_cast<size_t>(i)].c_str())) {
                    for (size_t k = 0; k < p.enumNames.size(); ++k)
                        if (ImGui::Selectable(p.enumNames[k].c_str(), static_cast<int>(k) == i)) commit(p, p.enumNames[k]);
                    ImGui::EndCombo();
                }
                break;
            }
            case PropertyType::AssetRef: {
                std::string s = std::get<std::string>(v);
                std::string shown = s.empty() ? "(none)" : m_session.registry().resolve(s);
                ImGui::SetNextItemWidth(-140);
                ImGui::InputText(label, &shown, ImGuiInputTextFlags_ReadOnly);
                ImGui::SameLine();
                if (ImGui::SmallButton("...")) ImGui::OpenPopup("pick");
                if (ImGui::BeginPopup("pick")) {
                    std::string picked;
                    if (assetPicker(AssetType::Texture, picked)) commit(p, picked);
                    ImGui::EndPopup();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetPayload))
                        if (auto* rec = m_session.registry().findByPath(static_cast<const char*>(pl->Data)); rec && rec->type == AssetType::Texture)
                            commit(p, rec->guid.toString());
                    ImGui::EndDragDropTarget();
                }
                break;
            }
            default: ImGui::TextDisabled("%s", label); break;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) m_session.history().closeMerge();
        if (!p.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.description.c_str());
        ImGui::PopID();
    }
    ImGui::TextDisabled("roughness/metallic: Blinn-Phong approximation; normal/MR textures not rendered yet.");
    ImGui::End();
}

void EditorApp::drawRenderSettings() {
    if (!ImGui::Begin("Render Settings", &panel("Render Settings")->open)) {
        ImGui::End();
        return;
    }
    RenderSettings& rs = m_session.world().renderSettings;
    const auto& frame = m_renderer->lastFrame();
    ImGui::TextDisabled("%s | %zu passes | %d cascades | %.1f ms", frame.fullPipeline ? "full pipeline" : "basic path", frame.passes.size(),
                        frame.cascades, 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));
    for (const auto& p : RenderSettings::typeInfo().properties) {
        ImGui::PushID(p.name.c_str());
        PropertyValue v = p.get(&rs);
        const char* label = p.name.c_str();
        switch (p.type) {
            case PropertyType::Bool: {
                bool b = std::get<bool>(v);
                if (ImGui::Checkbox(label, &b)) p.set(&rs, b);
                break;
            }
            case PropertyType::Int: {
                int i = std::get<int>(v);
                if (ImGui::InputInt(label, &i)) {
                    p.set(&rs, i);
                    rs.sanitize();
                }
                break;
            }
            case PropertyType::Float: {
                float f = std::get<float>(v);
                if (ImGui::SliderFloat(label, &f, p.minValue, p.maxValue)) p.set(&rs, f);
                break;
            }
            case PropertyType::Enum: {
                int i = std::get<int>(v);
                if (ImGui::BeginCombo(label, p.enumNames[static_cast<size_t>(i)].c_str())) {
                    for (size_t k = 0; k < p.enumNames.size(); ++k)
                        if (ImGui::Selectable(p.enumNames[k].c_str(), static_cast<int>(k) == i)) p.set(&rs, static_cast<int>(k));
                    ImGui::EndCombo();
                }
                break;
            }
            case PropertyType::AssetRef:
            case PropertyType::String: {
                std::string s = std::get<std::string>(v);
                std::string shown = s.empty() ? "(procedural sky)" : m_session.registry().resolve(s);
                ImGui::SetNextItemWidth(-140);
                ImGui::InputText(label, &shown, ImGuiInputTextFlags_ReadOnly);
                ImGui::SameLine();
                if (ImGui::SmallButton("...")) ImGui::OpenPopup("pick");
                if (ImGui::BeginPopup("pick")) {
                    if (ImGui::MenuItem("(procedural sky)")) p.set(&rs, std::string());
                    for (auto& r : m_session.registry().all())
                        if (r.type == AssetType::Texture && endsWith(toLower(r.path), ".hdr") && ImGui::MenuItem(r.path.c_str()))
                            p.set(&rs, r.guid.toString());
                    ImGui::EndPopup();
                }
                break;
            }
            default: break;
        }
        if (!p.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.description.c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button("Save as project defaults")) {
        auto st = m_session.saveRenderSettingsToProject();
        m_status = st ? "Render settings saved to the project" : st.error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert") && m_session.hasProject()) m_session.world().renderSettings = m_session.project().render;
    ImGui::TextDisabled("TAA: not implemented (FXAA only). Point-light shadows: not implemented.");
    if (ImGui::CollapsingHeader("Frame graph")) {
        for (auto& pass : frame.passes) ImGui::BulletText("%s", pass.c_str());
        for (auto& pass : frame.culled) ImGui::TextDisabled("  culled: %s", pass.c_str());
    }
    ImGui::End();
}

} // namespace fledge::editor
