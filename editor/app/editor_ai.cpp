// AI Assistant panel: message list with progressive reveal, tool-call cards
// with Undo links, provider/model selector, context chips and key status.
//
// The providers return whole responses (LlmProvider::chatStream is a stub that
// delivers the final text once), so the "streaming" look is a client-side reveal.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <vector>

#include "fledge/ai/claude_code.h"
#include "fledge/ai/presets.h"
#include "fledge/core/log.h"
#include "fledge/core/redact.h"
#include "fledge/editor/chat_markdown.h"
#include "editor_app.h"
#include "editor_icons.h"

namespace fledge::editor {

namespace {
double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr double kRevealCharsPerSecond = 140.0;

std::string compactArgs(const Json& j) {
    if (j.is_discarded() || j.is_null()) return {};
    std::string s = j.dump();
    return s.size() > 160 ? s.substr(0, 157) + "..." : s;
}

// ---- Markdown drawing for assistant replies (parser: fledge/editor/chat_markdown.h) --------------------------------
// One function measures and draws: dl == nullptr only measures. Text is laid out word by word so bold, code and links
// can mix inside a wrapped paragraph (ImGui's own wrapped text has one style per call).
struct MdStyle {
    ImFont* ui = nullptr;
    ImFont* mono = nullptr;
    float s = 1;       // UI scale
    ImU32 text = 0, dim = 0, accent = 0, codeBg = 0, line = 0;
    std::string* clickedLink = nullptr;
};

ImU32 mixColor(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w});
}

size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

// Lays out spans from (x0, y) inside [x0, x0 + wrapW]. Returns the y below the last line; *usedW gets the widest line.
float mdSpans(const std::vector<MdSpan>& spans, float x0, float y, float wrapW, ImDrawList* dl, const MdStyle& st, bool forceBold,
              ImU32 baseCol, float* usedW) {
    ImGui::PushFont(st.ui);
    const float lineH = ImGui::GetTextLineHeight() * 1.28f;
    ImGui::PopFont();
    float x = x0;
    bool lineHasText = false;
    auto newline = [&] {
        x = x0;
        y += lineH;
        lineHasText = false;
    };
    for (const MdSpan& sp : spans) {
        ImGui::PushFont(sp.code ? st.mono : st.ui);
        const bool bold = forceBold || sp.bold;
        ImU32 col = !sp.link.empty() ? st.accent : sp.italic ? mixColor(baseCol, st.accent, 0.35f) : baseCol;
        const float padX = sp.code ? 3 * st.s : 0;
        const char* p = sp.text.data();
        const char* end = p + sp.text.size();
        while (p < end) {
            // token = run of spaces, or run of non-spaces
            const bool space = *p == ' ';
            const char* q = p;
            while (q < end && (*q == ' ') == space) ++q;
            if (space) {
                if (lineHasText) x += ImGui::CalcTextSize(p, q).x;
                p = q;
                continue;
            }
            float w = ImGui::CalcTextSize(p, q).x + 2 * padX + (bold ? 0.7f * st.s : 0);
            if (lineHasText && x + w > x0 + wrapW) newline();
            if (w > wrapW) {  // a word longer than the line (URL, path): cut where it stops fitting
                const char* cut = p;
                float cw = 0;
                while (cut < q) {
                    size_t n = std::min<size_t>(utf8Len(static_cast<unsigned char>(*cut)), static_cast<size_t>(q - cut));
                    float add = ImGui::CalcTextSize(cut, cut + n).x;
                    if (cw + add + 2 * padX > wrapW && cut > p) break;
                    cw += add;
                    cut += n;
                }
                q = cut;
                w = cw + 2 * padX;
            }
            if (dl) {
                ImVec2 a{x, y}, b{x + w, y + lineH};
                if (sp.code) dl->AddRectFilled({a.x, a.y + 1}, {b.x, b.y - 2 * st.s}, st.codeBg, 3 * st.s);
                ImVec2 tp{x + padX, y + (lineH - ImGui::GetTextLineHeight()) * 0.5f};
                dl->AddText(tp, col, p, q);
                if (bold) dl->AddText({tp.x + 0.7f * st.s, tp.y}, col, p, q);  // no bold face is loaded: a second pass thickens it
                if (sp.strike) dl->AddLine({a.x, a.y + lineH * 0.52f}, {b.x, a.y + lineH * 0.52f}, col, 1);
                if (!sp.link.empty()) {
                    dl->AddLine({a.x, b.y - 3 * st.s}, {b.x, b.y - 3 * st.s}, col, 1);
                    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        // A link in a model's answer is never opened for the user: it is shown, and a click copies it.
                        ImGui::SetTooltip("%s\n(click to copy)", sp.link.c_str());
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && st.clickedLink) *st.clickedLink = sp.link;
                    }
                }
            }
            x += w;
            lineHasText = true;
            if (usedW) *usedW = std::max(*usedW, x - x0);
            p = q;
        }
        ImGui::PopFont();
    }
    return y + lineH;
}

ImVec2 mdLayout(const std::vector<MdBlock>& blocks, ImVec2 origin, float wrapW, ImDrawList* dl, const MdStyle& st) {
    float y = origin.y, usedW = 0;
    const float gap = 5 * st.s;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const MdBlock& b = blocks[i];
        if (i) y += b.kind == MdBlock::Kind::Heading ? gap * 2 : (b.kind == MdBlock::Kind::Bullet || b.kind == MdBlock::Kind::Numbered) &&
                                                                 blocks[i - 1].kind == b.kind ? gap * 0.3f : gap;
        switch (b.kind) {
            case MdBlock::Kind::Paragraph: y = mdSpans(b.spans, origin.x, y, wrapW, dl, st, false, st.text, &usedW); break;
            case MdBlock::Kind::Heading: {
                y = mdSpans(b.spans, origin.x, y, wrapW, dl, st, true, b.level <= 2 ? st.text : mixColor(st.text, st.dim, 0.3f), &usedW);
                if (b.level == 1) {
                    if (dl) dl->AddLine({origin.x, y}, {origin.x + wrapW, y}, st.line, 1);
                    usedW = wrapW;
                    y += 2 * st.s;
                }
                break;
            }
            case MdBlock::Kind::Bullet:
            case MdBlock::Kind::Numbered: {
                float indent = 14 * st.s * static_cast<float>(b.level) + 4 * st.s;
                float markW = 12 * st.s;
                ImGui::PushFont(st.ui);
                float lh = ImGui::GetTextLineHeight() * 1.28f;
                if (b.kind == MdBlock::Kind::Numbered) markW = ImGui::CalcTextSize(b.marker.c_str()).x + 6 * st.s;
                if (dl) {
                    if (b.kind == MdBlock::Kind::Bullet) dl->AddCircleFilled({origin.x + indent + 4 * st.s, y + lh * 0.5f}, 2.2f * st.s, st.dim);
                    else dl->AddText({origin.x + indent, y + (lh - ImGui::GetTextLineHeight()) * 0.5f}, st.dim, b.marker.c_str());
                }
                ImGui::PopFont();
                float w = 0;
                y = mdSpans(b.spans, origin.x + indent + markW, y, std::max(40 * st.s, wrapW - indent - markW), dl, st, false, st.text, &w);
                usedW = std::max(usedW, w + indent + markW);
                break;
            }
            case MdBlock::Kind::Quote: {
                float indent = 12 * st.s * static_cast<float>(b.level + 1);
                float y0 = y, w = 0;
                y = mdSpans(b.spans, origin.x + indent, y, std::max(40 * st.s, wrapW - indent), dl, st, false, st.dim, &w);
                if (dl) dl->AddRectFilled({origin.x + indent - 8 * st.s, y0 + 2}, {origin.x + indent - 5 * st.s, y - 2}, st.line, 1.5f * st.s);
                usedW = std::max(usedW, w + indent);
                break;
            }
            case MdBlock::Kind::Code: {
                float pad = 7 * st.s;
                ImGui::PushFont(st.mono);
                const char* cb = b.code.data();
                const char* ce = cb + b.code.size();
                ImVec2 ts = b.code.empty() ? ImVec2(0, ImGui::GetTextLineHeight()) : ImGui::CalcTextSize(cb, ce, false, wrapW - 2 * pad);
                if (dl) {
                    dl->AddRectFilled({origin.x, y}, {origin.x + wrapW, y + ts.y + 2 * pad}, st.codeBg, 5 * st.s);
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {origin.x + pad, y + pad}, st.text, cb, ce, wrapW - 2 * pad);
                }
                ImGui::PopFont();
                y += ts.y + 2 * pad;
                usedW = wrapW;
                break;
            }
            case MdBlock::Kind::Rule:
                if (dl) dl->AddLine({origin.x, y + 3 * st.s}, {origin.x + wrapW, y + 3 * st.s}, st.line, 1);
                y += 7 * st.s;
                usedW = wrapW;
                break;
            case MdBlock::Kind::Table: {
                size_t cols = 0;
                for (auto& r : b.rows) cols = std::max(cols, r.size());
                if (cols == 0) break;
                cols = std::min<size_t>(cols, 12);
                float colW = wrapW / static_cast<float>(cols), cp = 5 * st.s;
                for (size_t r = 0; r < b.rows.size(); ++r) {
                    float rowBottom = y;
                    for (int pass = 0; pass < 2; ++pass) {  // measure the row, then draw it at one height
                        if (pass == 1 && !dl) break;
                        for (size_t c = 0; c < std::min(cols, b.rows[r].size()); ++c) {
                            float yy = mdSpans(b.rows[r][c], origin.x + colW * static_cast<float>(c) + cp, y + cp * 0.5f, std::max(20 * st.s, colW - 2 * cp),
                                               pass ? dl : nullptr, st, r == 0, st.text, nullptr);
                            rowBottom = std::max(rowBottom, yy + cp * 0.5f);
                        }
                    }
                    y = rowBottom;
                    if (dl) dl->AddLine({origin.x, y}, {origin.x + wrapW, y}, r == 0 ? st.dim : st.line, 1);
                }
                usedW = wrapW;
                break;
            }
        }
    }
    return {usedW, y - origin.y};
}
} // namespace

void EditorApp::rebuildProvider() {
    auto& policy = m_mcp->policy();
    m_session.ai.setCategoryAllowed(AiCategory::Destructive, m_mcpAllowDestructive);
    m_session.ai.applyTo(policy);
    policy.readOnlyMode = policy.readOnlyMode || m_mcpReadOnly;
    auto key = ai::resolveApiKey(*m_creds, m_aiConfig.resolvedCredentialId());
    m_keySource = key.source;
    m_assistant.reset();
    m_provider.reset();
    m_session.ai.setProviderInfo(Json(nullptr));
    if (!aiOn()) {  // master switch: no provider object exists, so no network call can happen
        key.key.assign(key.key.size(), '\0');
        return;
    }
    if (m_aiConfig.needsKey() && key.key.empty()) return;
    // Endpoint checks: errors stop the provider (nothing is ever sent); warnings are logged and shown in Settings.
    ai::EndpointCheck chk = ai::validateEndpoint(m_aiConfig);
    for (auto& w : chk.warnings) FLEDGE_LOG_WARN("ai", "endpoint ({}): {}", m_aiConfig.describe().value("host", std::string()), w);
    if (!chk.ok()) {
        FLEDGE_LOG_ERROR("ai", "endpoint settings rejected: {}", chk.errors.front());
        key.key.assign(key.key.size(), '\0');
        return;
    }
    if (m_aiConfig.kind == ai::ProviderKind::ClaudeCode) {
        // The user's own Claude Code CLI does the turn. It reaches the editor tools through the MCP server, so that has
        // to be listening; no key and no HTTP request from the editor are involved.
        key.key.assign(key.key.size(), '\0');
        if (!m_mcpHttp.running()) startMcpHttp(m_mcpPortSetting);
        ai::ClaudeCodeOptions o;
        o.cliPath = m_aiConfig.cliPath;
        o.model = m_aiConfig.model;
        o.showThinking = m_aiConfig.showThinking;
        if (m_mcpHttp.running()) o.mcpUrl = std::format("http://127.0.0.1:{}/mcp", m_mcpHttp.port());
        if (const char* tok = std::getenv("FLEDGE_MCP_TOKEN"); tok && *tok) o.mcpToken = tok;
        o.workDir = m_session.hasProject() ? m_session.project().rootDir() : m_opt.settingsDir;
        o.scratchDir = m_opt.settingsDir / "ClaudeCode";
        m_provider = ai::createClaudeCodeProvider(std::move(o));
        m_session.ai.setProviderInfo(m_aiConfig.describe());
        m_assistant = std::make_unique<ai::AssistantSession>(*m_provider, std::vector<ai::ToolSpec>{}, [](const std::string& name, const Json&) {
            return mcp::ToolResult::error("'" + name + "' is not run here: Claude Code calls the editor through MCP.");
        });
        return;
    }
    m_provider = ai::createProvider(m_aiConfig, key.key, *m_http);
    key.key.assign(key.key.size(), '\0');
    m_session.ai.setProviderInfo(m_aiConfig.describe());
    auto specs = ai::AssistantSession::toolSpecsFrom(m_tools, m_mcp->policy());
    const bool toolsOk = m_aiConfig.supportsTools();
    if (!toolsOk) specs.clear();  // chat-only model: never send tool definitions
    m_assistant = std::make_unique<ai::AssistantSession>(*m_provider, specs, [this](const std::string& name, const Json& args) {
        // AI control level: ask the user on the main thread while this worker waits.
        if (const mcp::Tool* tool = m_tools.find(name)) {
            mcp::Tool view = aiAuthorizationView(*tool, args);
            AiDecision d = m_session.ai.evaluate(view);
            if (d.needsApproval && !requestAiApproval(view, args))
                return mcp::ToolResult::error("The user denied '" + name + "' (AI control: approval required).");
        }
        return m_mainQueue.call<mcp::ToolResult>([this, name, args] {
            size_t before = m_session.history().cursor();
            m_session.aiCaller = "assistant";
            mcp::ToolResult r = m_tools.call(name, args, m_mcp->policy());
            size_t after = m_session.history().cursor();
            if (after > before) {  // the tool recorded an undoable "AI: <tool>" command
                std::lock_guard l(m_chatMutex);
                for (auto it = m_chat.rbegin(); it != m_chat.rend(); ++it)
                    if (it->kind == ChatLine::Kind::Tool && it->toolName == name && !it->toolDone) {
                        it->historyIndex = static_cast<int>(after) - 1;
                        break;
                    }
            }
            return r;
        });
    });
    if (!toolsOk) m_assistant->setSystemPrompt(ai::AssistantSession::chatOnlySystemPrompt());
}

void EditorApp::sendAssistant(const std::string& userText) {
    if (!aiOn()) return;  // master switch
    if (!m_assistant) {
        std::lock_guard l(m_chatMutex);
        m_chat.push_back({ChatLine::Kind::Error, "No AI provider is ready. Open AI > Provider Settings to pick a provider and store a key."});
        return;
    }
    if (m_aiBusy) return;
    if (m_aiThread.joinable()) m_aiThread.join();
    // Context chips -> a short, explicit context block (names and ids only; never file contents or secrets).
    std::string context;
    if (m_session.hasProject()) {
        if (m_ctxProject) context += std::format("Project: {} (startup scene {})\n", m_session.project().name, m_session.project().startupScene);
        if (m_ctxScene)
            context += std::format("Scene: {} with {} entities\n", m_session.scene().name, m_session.scene().entities().size());
        if (m_ctxSelection && !m_session.selection.empty()) {
            context += "Selected:";
            for (EntityId e : m_session.selection) context += std::format(" {} (#{})", m_session.scene().displayName(e), e);
            context += "\n";
        }
    }
    std::string text = context.empty() ? userText : "[Editor context]\n" + context + "[/Editor context]\n" + userText;
    {
        std::lock_guard l(m_chatMutex);
        ChatLine u{ChatLine::Kind::User, userText};
        m_chat.push_back(u);
        m_chatStick = true;  // sending always returns to the newest message
    }
    m_aiBusy = true;
    m_aiThread = std::thread([this, text] {
        m_assistant->send(text, [this](const ai::AssistantEvent& ev) {
            std::lock_guard l(m_chatMutex);
            switch (ev.kind) {
                case ai::AssistantEvent::Kind::AssistantText: {
                    ChatLine c{ChatLine::Kind::Assistant, ev.text};
                    c.revealStart = nowSeconds();
                    m_chat.push_back(std::move(c));
                    break;
                }
                case ai::AssistantEvent::Kind::Thinking: m_chat.push_back({ChatLine::Kind::Thinking, ev.text}); break;
                case ai::AssistantEvent::Kind::ToolCall: {
                    ChatLine c{ChatLine::Kind::Tool, ev.text};
                    c.toolName = ev.toolName;
                    c.toolArgs = compactArgs(ev.toolArgs);
                    m_chat.push_back(std::move(c));
                    break;
                }
                case ai::AssistantEvent::Kind::ToolResult: {
                    for (auto it = m_chat.rbegin(); it != m_chat.rend(); ++it)
                        if (it->kind == ChatLine::Kind::Tool && it->toolName == ev.toolName && !it->toolDone) {
                            it->toolDone = true;
                            it->toolError = ev.isError;
                            it->toolResult = ev.text.size() > 400 ? ev.text.substr(0, 400) + "..." : ev.text;
                            break;
                        }
                    break;
                }
                case ai::AssistantEvent::Kind::Notice: m_chat.push_back({ChatLine::Kind::Info, ev.text}); break;
                case ai::AssistantEvent::Kind::Error: m_chat.push_back({ChatLine::Kind::Error, ev.text}); break;
                case ai::AssistantEvent::Kind::Done: break;
            }
        });
        m_aiBusy = false;
    });
}

void EditorApp::drawToolCard(ChatLine& line, int index) {
    float s = effectiveScale();
    ImGui::PushID(index);
    ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, frame);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * s, 6 * s});
    ImGui::BeginChild("card", {ImGui::GetContentRegionAvail().x * 0.92f, 0},
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    ImVec4 col = !line.toolDone ? ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)
                 : line.toolError ? ImVec4(1, 0.45f, 0.45f, 1)
                                  : ImVec4(0.35f, 0.85f, 0.45f, 1);
    const char* icon = !line.toolDone ? ICON_SPINNER : line.toolError ? ICON_ERROR : ICON_CHECK;
    ImGui::TextColored(col, "%s", icon);
    ImGui::SameLine();
    ImGui::PushFont(m_fontMono);
    ImGui::TextUnformatted(line.toolName.c_str());
    ImGui::PopFont();
    if (line.historyIndex >= 0) {
        auto& h = m_session.history();
        size_t idx = static_cast<size_t>(line.historyIndex);
        bool exists = idx < h.commands().size();
        bool applied = exists && h.cursor() > idx;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 110 * s);
        if (line.undone || !applied) {
            ImGui::TextDisabled("%s undone", ICON_UNDO);
        } else if (h.cursor() == idx + 1) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            if (ImGui::SmallButton(ICON_UNDO " Undo") && !m_session.playing()) {
                m_session.undo();
                line.undone = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo \"%s\"", h.commands()[idx].label.c_str());
        } else {
            size_t steps = h.cursor() - idx;
            if (ImGui::SmallButton(ICON_UNDO " Undo...")) ImGui::OpenPopup("undoTo");
            if (ImGui::BeginPopup("undoTo")) {
                ImGui::Text("%zu newer edits were made after this tool call.", steps - 1);
                if (ImGui::Button(std::format("Undo {} steps (including newer edits)", steps).c_str())) {
                    for (size_t i = 0; i < steps; ++i) m_session.undo();
                    line.undone = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TextDisabled("Or pick a step in the History panel.");
                ImGui::EndPopup();
            }
        }
    }
    if (!line.toolArgs.empty()) {
        ImGui::PushFont(m_fontMono);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", line.toolArgs.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    if (line.toolDone && !line.toolResult.empty()) {
        if (ImGui::TreeNodeEx("result", ImGuiTreeNodeFlags_SpanAvailWidth | (line.toolError ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                              "%s", line.toolError ? "Error" : "Result")) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(line.toolResult.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void EditorApp::drawAssistant() {
    PanelDef* p = panel("AI Assistant");
    if (!ImGui::Begin("AI Assistant", p ? &p->open : nullptr)) {
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    // ---- header: provider / model / key status
    const char* kinds[] = {"Anthropic", "OpenAI", "OpenAI-compatible", "Custom", "Claude Code"};
    int k = static_cast<int>(m_aiConfig.kind);
    ImGui::BeginDisabled(m_aiBusy);
    ImGui::SetNextItemWidth(120 * s);
    if (ImGui::Combo("##prov", &k, kinds, 5)) {
        m_aiConfig.kind = static_cast<ai::ProviderKind>(k);
        m_aiConfig.model.clear();
        m_aiConfig.baseUrl.clear();
        m_aiConfig.credentialId.clear();  // a preset's credential id must not follow us to another provider
        m_aiConfig.presetId.clear();
        m_aiConfig.custom = {};
        m_extraBodyTextInit = false;
        saveSettings();
        rebuildProvider();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 150 * s));
    ImGui::InputTextWithHint("##model", m_aiConfig.resolvedModel().c_str(), &m_aiConfig.model);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        saveSettings();
        rebuildProvider();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Model id (empty = provider default: %s)", m_aiConfig.resolvedModel().c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool needsKey = m_aiConfig.needsKey();
    bool keyOk = !needsKey || m_keySource != ai::KeySource::None;
    ImGui::PushStyleColor(ImGuiCol_Text, keyOk ? ImVec4(0.35f, 0.85f, 0.45f, 1) : ImVec4(1, 0.65f, 0.3f, 1));
    if (ImGui::SmallButton(keyOk ? ICON_LOCK : ICON_KEY)) m_commands.execute("ai.settings");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", !needsKey ? "This provider needs no key (local server)"
                                : m_keySource == ai::KeySource::Store       ? "API key: stored in the credential store (value hidden)"
                                : m_keySource == ai::KeySource::Environment ? "API key: from an environment variable (value hidden)"
                                                                            : "No API key - click to configure");
    ImGui::SameLine();
    if (!m_aiConfig.supportsTools()) {
        ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%s chat only", ICON_WARNING);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This model is marked as not tool-capable: it can chat, but it cannot use the editor tools.\n"
                              "Pick a tool-capable model in Settings > AI Providers to let the assistant edit the scene.");
        ImGui::SameLine();
    }
    if (ImGui::SmallButton(ICON_COMMENT)) m_commands.execute("ai.new_chat");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New chat");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_GEAR)) m_commands.execute("ai.settings");
    ImGui::Separator();

    // ---- messages
    float inputH = ImGui::GetFrameHeightWithSpacing() * 3.6f;
    ImGui::BeginChild("chat", {0, -inputH}, ImGuiChildFlags_None);
    bool anyRevealing = false;
    {
        std::lock_guard l(m_chatMutex);
        bool demo = std::any_of(m_chat.begin(), m_chat.end(), [](const ChatLine& c) { return c.demo; });
        if (demo)
            ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%s Demo transcript (--ui-demo-chat): canned messages, no provider was called.", ICON_INFO);
        if (m_chat.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s  Ask the assistant to build or change your scene.", ICON_WAND);
            ImGui::TextDisabled("It uses the same engine tools as the MCP server; every edit is undoable.");
            ImGui::Spacing();
            for (const char* sug : {"Add three red cubes in a row and a point light", "Light this scene like a sunset",
                                    "List the entities in this scene", "Put a ground plane under everything"}) {
                if (ImGui::Button(sug)) m_chatInput = sug;
            }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        double now = nowSeconds();
        for (int i = 0; i < static_cast<int>(m_chat.size()); ++i) {
            ChatLine& line = m_chat[i];
            float avail = ImGui::GetContentRegionAvail().x;
            float pad = 8 * s;
            if (line.kind == ChatLine::Kind::Tool) {
                ImGui::Indent(24 * s);
                drawToolCard(line, i);
                ImGui::Unindent(24 * s);
                ImGui::Spacing();
                continue;
            }
            if (line.kind == ChatLine::Kind::Thinking) {
                // The model's reasoning summary: folded by default, dim when open, never Markdown-rendered.
                ImGui::PushID(i);
                ImGui::Indent(24 * s);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::SetNextItemOpen(line.open);
                line.open = ImGui::TreeNodeEx("##thinking", ImGuiTreeNodeFlags_SpanAvailWidth, "Thinking");
                if (line.open) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail * 0.84f);
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::TreePop();
                }
                ImGui::PopStyleColor();
                ImGui::Unindent(24 * s);
                ImGui::PopID();
                ImGui::Dummy({1, 4 * s});
                continue;
            }
            std::string text = line.text;
            bool revealing = false;
            if (line.kind == ChatLine::Kind::Assistant && line.revealStart > 0 && !line.demo) {
                size_t n = static_cast<size_t>((now - line.revealStart) * kRevealCharsPerSecond);
                if (n < text.size()) {
                    while (n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) ++n;  // never cut inside a UTF-8 character
                    text = text.substr(0, n);
                    anyRevealing = revealing = true;
                }
            }
            bool user = line.kind == ChatLine::Kind::User;
            float maxW = avail * (user ? 0.80f : 0.88f);
            // Assistant replies are Markdown; everything else (what the user typed, errors, notes) is shown as written.
            const bool markdown = line.kind == ChatLine::Kind::Assistant;
            std::vector<MdBlock> blocks;
            MdStyle md;
            std::string clickedLink;
            ImVec2 ts;
            if (markdown) {
                blocks = parseChatMarkdown(text);
                if (revealing) {
                    if (blocks.empty() || blocks.back().kind == MdBlock::Kind::Code || blocks.back().kind == MdBlock::Kind::Table ||
                        blocks.back().kind == MdBlock::Kind::Rule)
                        blocks.push_back({});
                    blocks.back().spans.push_back({" " ICON_CIRCLE});
                }
                md.ui = m_fontUi;
                md.mono = m_fontMono;
                md.s = s;
                md.text = ImGui::GetColorU32(ImGuiCol_Text);
                md.dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                md.accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
                md.codeBg = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.75f);
                md.line = ImGui::GetColorU32(ImGuiCol_Border);
                md.clickedLink = &clickedLink;
                ts = mdLayout(blocks, {0, 0}, maxW - 2 * pad, nullptr, md);
            } else {
                ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, maxW - 2 * pad);
            }
            float x0 = ImGui::GetCursorPosX();
            if (!user) {
                const char* icon = line.kind == ChatLine::Kind::Assistant ? ICON_ROBOT : line.kind == ChatLine::Kind::Error ? ICON_ERROR : ICON_INFO;
                ImVec4 ic = line.kind == ChatLine::Kind::Error ? ImVec4(1, 0.45f, 0.45f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                ImGui::TextColored(ic, "%s", icon);
                ImGui::SameLine(x0 + 24 * s);
            } else {
                ImGui::SetCursorPosX(x0 + avail - ts.x - 2 * pad);
            }
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1{p0.x + ts.x + 2 * pad, p0.y + ts.y + 2 * pad};
            ImU32 bg = user ? ImGui::GetColorU32(ImGuiCol_Header)
                       : line.kind == ChatLine::Kind::Error ? IM_COL32(200, 60, 60, 50)
                                                            : ImGui::GetColorU32(ImGuiCol_FrameBg);
            if (line.kind != ChatLine::Kind::Info) dl->AddRectFilled(p0, p1, bg, 8 * s);
            ImGui::SetCursorScreenPos({p0.x + pad, p0.y + pad});
            if (markdown) {
                if (ImGui::IsRectVisible(p0, p1)) mdLayout(blocks, {p0.x + pad, p0.y + pad}, maxW - 2 * pad, dl, md);
                if (!clickedLink.empty()) {
                    ImGui::SetClipboardText(clickedLink.c_str());
                    m_status = "Link copied";
                }
            } else {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxW - 2 * pad);
                if (line.kind == ChatLine::Kind::Info) ImGui::TextDisabled("%s", text.c_str());
                else ImGui::TextUnformatted(text.c_str());
                ImGui::PopTextWrapPos();
            }
            // Right-click a message to copy it (assistant: as Markdown, or as plain text).
            ImGui::PushID(i);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("msg");
            if (ImGui::BeginPopup("msg")) {
                if (ImGui::MenuItem(markdown ? "Copy as Markdown" : "Copy")) ImGui::SetClipboardText(line.text.c_str());
                if (markdown && ImGui::MenuItem("Copy as plain text")) ImGui::SetClipboardText(markdownPlainText(parseChatMarkdown(line.text)).c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos({p0.x, p1.y});
            ImGui::Dummy({1, 6 * s});
        }
        if (m_aiBusy) {
            int dots = static_cast<int>(ImGui::GetTime() * 3) % 4;
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", ICON_ROBOT);
            ImGui::SameLine();
            ImGui::TextDisabled("thinking%.*s", dots, "...");
        }
    }
    // Follow new output only while the view is at the bottom. Scrolling up lets go; a pill then says that there is more.
    {
        const float scrollY = ImGui::GetScrollY(), maxY = ImGui::GetScrollMaxY();
        if (scrollY < m_chatLastScrollY - 0.5f) m_chatStick = false;  // moved up (wheel, scrollbar, keys); growth never does that
        if (scrollY >= maxY - 4 * s) m_chatStick = true;
        size_t count;
        {
            std::lock_guard l(m_chatMutex);
            count = m_chat.size();
        }
        if (m_chatStick) {
            m_chatSeen = count;
            if (anyRevealing || m_aiBusy || scrollY < maxY) ImGui::SetScrollHereY(1.0f);
        } else if (count > m_chatSeen || anyRevealing || m_aiBusy) {
            size_t unseen = count > m_chatSeen ? count - m_chatSeen : 0;
            std::string label = unseen == 0 ? std::string(ICON_CIRCLE "  Jump to latest")
                                            : std::format("{}  {} new message{}", ICON_CIRCLE, unseen, unseen == 1 ? "" : "s");
            ImVec2 back = ImGui::GetCursorPos();
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            float bw = ImGui::CalcTextSize(label.c_str()).x + 24 * s, bh = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos({wp.x + (ws.x - bw) * 0.5f, wp.y + ws.y - bh - 10 * s});
            ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a.x, a.y, a.z, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(a.x, a.y, a.z, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.06f, 1));
            if (ImGui::Button(label.c_str(), {bw, bh})) {
                m_chatStick = true;
                ImGui::SetScrollY(maxY);
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            ImGui::SetCursorPos(back);
        }
        m_chatLastScrollY = ImGui::GetScrollY();
    }
    ImGui::EndChild();

    // ---- context chips
    auto chip = [&](const char* id, const std::string& label, bool& on, const char* tip) {
        ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8 * s, 2 * s});
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(a.x, a.y, a.z, 0.30f) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        std::string l = (on ? std::string(ICON_PAPERCLIP " ") : std::string(ICON_XMARK " ")) + label + "###" + id;
        float w = ImGui::CalcTextSize(l.c_str(), nullptr, true).x + 16 * s;
        if (ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x + 1 && ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
        if (ImGui::SmallButton(l.c_str())) on = !on;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(click to %s)", tip, on ? "detach" : "attach");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::SameLine();
    };
    if (m_session.hasProject()) {
        chip("scene", std::format("Scene: {} ({})", m_session.scene().name, m_session.scene().entities().size()), m_ctxScene,
             "Attach the scene name and entity count");
        std::string sel = m_session.selection.empty() ? "Selection: none"
                          : m_session.selection.size() == 1
                              ? "Selection: " + m_session.scene().displayName(m_session.selection[0])
                              : std::format("Selection: {} (+{})", m_session.scene().displayName(m_session.primarySelection()),
                                            m_session.selection.size() - 1);
        chip("sel", sel, m_ctxSelection, "Attach the selected entity names and ids");
        chip("proj", "Project: " + m_session.project().name, m_ctxProject, "Attach the project name and startup scene");
    }
    ImGui::NewLine();

    // ---- input
    ImGui::BeginDisabled(m_aiBusy);
    float sendW = ImGui::CalcTextSize(ICON_SEND " Send").x + ImGui::GetStyle().FramePadding.x * 2 + 4;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x);
    bool enter = ImGui::InputTextWithHint("##msg", m_assistant ? "Ask for a change, e.g. add a red cube on the left" : "Configure a provider to chat",
                                          &m_chatInput, ImGuiInputTextFlags_EnterReturnsTrue);
    trackRect("ai.input");
    ImGui::SameLine();
    if ((ImGui::Button(ICON_SEND " Send") || enter) && !m_chatInput.empty()) {
        sendAssistant(m_chatInput);
        m_chatInput.clear();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// ------------------------------------------------------------ AI master switch --
void EditorApp::setAiEnabled(bool on) {
    if (on == aiOn()) return;
    if (!on) {
        // stop everything AI first: MCP server, pending approvals, the assistant worker
        m_mcpHttp.stop();
        {
            std::lock_guard l(m_approvalMutex);
            for (auto& a : m_approvals) a.answer->set_value(0);
            m_approvals.clear();
        }
        m_session.ai.setEnabled(false);
        while (m_aiBusy) {  // the worker may wait for main-thread tool calls; they are refused now
            m_mainQueue.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (m_aiThread.joinable()) m_aiThread.join();
        if (m_prefs.workspace == "AI") switchWorkspace("Level");
        rebuildProvider();  // drops provider + assistant
        toast(ToastLevel::Info, "AI features are OFF: no assistant, no MCP server, no AI network calls");
    } else {
        m_session.ai.setEnabled(true);
        rebuildProvider();
        if (m_mcpAutoStart) startMcpHttp(m_mcpPortSetting);
        toast(ToastLevel::Success, std::string("AI features are ON (control level: ") + toString(m_session.ai.level()) + ")");
    }
    m_session.ai.applyTo(m_mcp->policy());
    saveSettings();
}

bool EditorApp::requestAiApproval(const mcp::Tool& tool, const Json& args) {
    if (!aiOn() || m_shuttingDown) return false;
    AiApproval a;
    a.tool = tool.name;
    a.title = tool.title;
    a.category = toString(aiCategoryOf(tool));
    std::string dumped = args.dump();
    a.args = dumped.size() > 400 ? dumped.substr(0, 400) + "..." : dumped;
    a.answer = std::make_shared<std::promise<int>>();
    auto fut = a.answer->get_future();
    {
        std::lock_guard l(m_approvalMutex);
        m_approvals.push_back(std::move(a));
    }
    if (fut.wait_for(std::chrono::seconds(120)) != std::future_status::ready) return false;
    int r = fut.get();
    if (r == 1) m_session.ai.grantOnce(tool.name);
    if (r == 2) m_session.ai.grantForSession(tool.name);
    return r != 0;
}

void EditorApp::drawAiApproval() {
    AiApproval* cur = nullptr;
    {
        std::lock_guard l(m_approvalMutex);
        if (!m_approvals.empty()) cur = &m_approvals.front();
    }
    if (!cur) return;
    if (!ImGui::IsPopupOpen("AI approval")) ImGui::OpenPopup("AI approval");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({520 * effectiveScale(), 0});
    if (!ImGui::BeginPopupModal("AI approval", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s The AI wants to run an action", ICON_ROBOT);
    ImGui::Separator();
    ImGui::Text("%s  (%s)", cur->title.c_str(), cur->tool.c_str());
    ImGui::TextDisabled("category: %s   control level: %s", cur->category.c_str(), toString(m_session.ai.level()));
    ImGui::PushFont(m_fontMono);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", cur->args.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextDisabled("Approved changes are recorded in History as [AI] and can be undone.");
    int answer = -1;
    if (ImGui::Button(ICON_CHECK " Allow once")) answer = 1;
    trackRect("approval.once");
    ImGui::SameLine();
    if (ImGui::Button("Allow for this session")) answer = 2;
    trackRect("approval.session");
    ImGui::SameLine();
    if (ImGui::Button(ICON_XMARK " Deny")) answer = 0;
    trackRect("approval.deny");
    if (answer >= 0) {
        std::lock_guard l(m_approvalMutex);
        if (!m_approvals.empty()) {
            m_approvals.front().answer->set_value(answer);
            m_approvals.pop_front();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::drawAiControlSettings() {
    float s = effectiveScale();
    bool on = aiOn();
    if (ImGui::Checkbox(ICON_ROBOT "  Enable AI features", &on)) setAiEnabled(on);
    trackRect("settings.ai.enable");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Per-user master switch. Off = no AI Assistant, no AI workspace or menus, the MCP server is stopped "
                        "(HTTP port closed; fledge_mcp_server refuses to start) and no AI provider/network calls are made. "
                        "Project files cannot change this.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!on);
    ImGui::SeparatorText("AI control level");
    int lv = static_cast<int>(m_session.ai.level());
    const char* names[] = {"Off - the AI may only read", "Ask first - approve every change", "Auto-approve safe actions",
                           "Full control"};
    const char* tips[] = {"Inspecting the project, scene, logs and docs only. Every change is refused.",
                          "Each change opens an approval prompt: Allow once / Allow for this session / Deny.",
                          "Undoable scene, asset and script edits run automatically; the risky categories below ask first.",
                          "Everything permitted below runs without asking."};
    for (int i = 0; i < 4; ++i) {
        if (ImGui::RadioButton(names[i], &lv, i)) {
            m_session.ai.setLevel(static_cast<AiLevel>(i));
            m_session.ai.applyTo(m_mcp->policy());
            saveSettings();
        }
        ImGui::SameLine(300 * s);
        ImGui::TextDisabled("%s", tips[i]);
    }
    ImGui::SeparatorText("Risky actions the AI may use at all");
    struct Cat {
        AiCategory c;
        const char* label;
        const char* tip;
    };
    for (Cat c : {Cat{AiCategory::Destructive, "Destructive tools", "delete entities, remove components, replace the scene"},
                  Cat{AiCategory::ScriptBuild, "Script builds", "compile and load Fledge Script files: native code with your rights"},
                  Cat{AiCategory::Download, "Downloads", "fetch SDKs / upscaler versions (licence is shown to you first)"},
                  Cat{AiCategory::Packaging, "Packaging", "cook content and package the game"}}) {
        bool allowed = m_session.ai.categoryAllowed(c.c);
        if (ImGui::Checkbox(c.label, &allowed)) {
            m_session.ai.setCategoryAllowed(c.c, allowed);
            if (c.c == AiCategory::Destructive) m_mcpAllowDestructive = allowed;
            m_session.ai.applyTo(m_mcp->policy());
            saveSettings();
        }
        ImGui::SameLine(300 * s);
        ImGui::TextDisabled("%s", c.tip);
    }
    if (ImGui::Button("Forget 'allow for this session' answers")) m_session.ai.clearSessionGrants();
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s Secrets (API keys, tokens) are never readable by any AI tool at any level. Every AI change is tagged "
                        "[AI] in History and can be undone. Everything the AI can do is also a normal command (menus, Ctrl+Shift+P).",
                        ICON_LOCK);
    ImGui::PopTextWrapPos();
}

// MCP over HTTP: approval prompts must happen on the HTTP thread, before the call is queued to the main thread.
std::string EditorApp::mcpPreApprove(const std::string& body) {
    Json j = Json::parse(body, nullptr, false);
    if (!j.is_object() || j.value("method", "") != "tools/call" || !j.contains("params")) return {};
    std::string name = j["params"].value("name", "");
    const mcp::Tool* tool = m_tools.find(name);
    if (!tool) return {};
    Json args = j["params"].value("arguments", Json::object());
    mcp::Tool view = aiAuthorizationView(*tool, args);  // e.g. migrate_settings apply is asked about like a destructive tool
    AiDecision d = m_session.ai.evaluate(view);
    if (!d.needsApproval) return {};
    if (requestAiApproval(view, args)) return {};
    Json result = {{"content", Json::array({{{"type", "text"}, {"text", "The user denied '" + name + "' (AI control: approval required)."}}})},
                   {"isError", true}};
    return mcp::rpc::makeResult(j.value("id", Json()), result).dump();
}

// ------------------------------------------------------------------------------------------------
// Settings > AI Providers: preset dropdown, provider/custom endpoint form, key storage, Test connection.
// Nothing here ever shows or stores a key outside the credential store.
// ------------------------------------------------------------------------------------------------
void EditorApp::startTestConnection() {
    if (!aiOn() || m_connBusy) return;  // master switch: no request without AI on
    if (m_connThread.joinable()) m_connThread.join();
    ai::ProviderConfig cfg = m_aiConfig;
    auto key = ai::resolveApiKey(*m_creds, cfg.resolvedCredentialId());
    m_connBusy = true;
    {
        std::lock_guard l(m_connMutex);
        m_connHasResult = false;
    }
    m_connThread = std::thread([this, cfg, k = std::move(key.key)]() mutable {
        ai::ConnectionTestResult r = ai::testConnection(cfg, k, m_httpRetry ? m_httpRetry->inner() : *m_http);
        k.assign(k.size(), '\0');
        std::lock_guard l(m_connMutex);
        m_connResult = std::move(r);
        m_connHasResult = true;
        m_connBusy = false;
    });
}

void EditorApp::drawAiProviderSettings() {
    float s = effectiveScale();
    const ImVec4 okCol{0.35f, 0.85f, 0.45f, 1}, warnCol{1, 0.75f, 0.3f, 1}, errCol{1, 0.45f, 0.45f, 1};
    bool edited = false;  // a preset was picked: save + rebuild
    ImGui::BeginDisabled(m_aiBusy);

    // ---- preset dropdown (fills the form; nothing is sent until Apply / Test connection)
    const auto& presets = ai::providerPresets();
    const ai::ProviderPreset* cur = m_aiConfig.presetId.empty() ? nullptr : ai::findPreset(m_aiConfig.presetId);
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::BeginCombo("Preset", cur ? cur->displayName.c_str() : "(none - choose below)")) {
        if (ImGui::Selectable("(none - manual setup)", cur == nullptr)) m_aiConfig.presetId.clear();
        for (const auto& p : presets) {
            if (ImGui::Selectable(p.displayName.c_str(), cur == &p)) {
                m_aiConfig = p.makeConfig();
                m_extraBodyTextInit = false;
                std::lock_guard l(m_connMutex);
                m_connHasResult = false;
                edited = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fills provider, URL, auth, key id and model list. Presets are unverified: check the provider's docs.");
    cur = m_aiConfig.presetId.empty() ? nullptr : ai::findPreset(m_aiConfig.presetId);
    if (cur) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", cur->notes.c_str());
        ImGui::PopTextWrapPos();
    }

    // ---- provider kind
    const char* kinds[] = {"anthropic", "openai", "openai-compatible", "custom", "claude code (your own CLI)"};
    int k = static_cast<int>(m_aiConfig.kind);
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::Combo("Provider", &k, kinds, 5) && static_cast<ai::ProviderKind>(k) != m_aiConfig.kind) {
        m_aiConfig.kind = static_cast<ai::ProviderKind>(k);
        m_aiConfig.presetId.clear();
        m_aiConfig.credentialId.clear();
        m_aiConfig.custom = {};
        m_extraBodyTextInit = false;
        cur = nullptr;
    }

    // ---- model (free text + the preset's list)
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::InputTextWithHint("Model", m_aiConfig.resolvedModel().empty() ? "model id your endpoint expects" : m_aiConfig.resolvedModel().c_str(),
                                 &m_aiConfig.model)) {
        if (cur)
            if (const ai::PresetModel* pm = cur->findModel(m_aiConfig.model)) m_aiConfig.custom.toolsEnabled = pm->toolCapable;
    }
    if (cur && !cur->models.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##presetmodels", "", ImGuiComboFlags_NoPreview)) {
            for (const auto& pm : cur->models) {
                std::string label = pm.id + (pm.toolCapable ? "" : "   (chat only, no tools)");
                if (ImGui::Selectable(label.c_str(), m_aiConfig.model == pm.id)) {
                    m_aiConfig.model = pm.id;
                    m_aiConfig.custom.toolsEnabled = pm.toolCapable;
                }
                if (ImGui::IsItemHovered() && !pm.note.empty()) ImGui::SetTooltip("%s", pm.note.c_str());
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Models listed by this preset");
    }

    const bool custom = m_aiConfig.kind == ai::ProviderKind::Custom;
    const bool claudeCode = m_aiConfig.kind == ai::ProviderKind::ClaudeCode;
    if (claudeCode) {
        ImGui::SetNextItemWidth(360 * s);
        ImGui::InputTextWithHint("Claude Code path", "empty = find `claude` automatically", &m_aiConfig.cliPath);
        ai::ClaudeCodeStatus cc = ai::findClaudeCode(m_aiConfig.cliPath);
        ImGui::PushTextWrapPos(0);
        if (cc.found) ImGui::TextColored({0.35f, 0.85f, 0.45f, 1}, "%s Found: %s", ICON_CHECK, cc.path.c_str());
        else ImGui::TextColored({1, 0.65f, 0.3f, 1}, "%s %s", ICON_WARNING, cc.message.c_str());
        ImGui::TextDisabled("Runs your own Claude Code program with your own sign-in (run `claude`, then /login, in a terminal). "
                            "The editor never sees that sign-in and sends nothing itself. Claude Code uses the editor through the MCP "
                            "server, which is started for it; file-writing and shell tools are not allowed from here. "
                            "Model: leave empty to use Claude Code's own setting.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::SetNextItemWidth(360 * s);
        ImGui::InputTextWithHint("Base URL", m_aiConfig.resolvedBaseUrl().empty() ? "https://host/v1" : m_aiConfig.resolvedBaseUrl().c_str(),
                                 &m_aiConfig.baseUrl);
    }
    ImGui::Checkbox("Show the model's thinking when it provides it", &m_aiConfig.showThinking);
    if (m_aiConfig.kind == ai::ProviderKind::Anthropic) {
        ImGui::SetNextItemWidth(260 * s);
        ImGui::InputTextWithHint("Effort", "default (low|medium|high|xhigh|max)", &m_aiConfig.effort);
        ImGui::Checkbox("Server-side fallback on refusal", &m_aiConfig.serverFallbacks);
        ImGui::Checkbox("Prompt caching (cheaper, faster follow-up turns)", &m_aiConfig.promptCaching);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Each turn resends the tool list, the instructions and the whole chat. With caching, the part that did not\n"
                              "change is read from Anthropic's cache at a fraction of the input price. Only for api.anthropic.com.");
    }

    // ---- custom endpoint form
    if (custom) {
        ai::CustomEndpoint& c = m_aiConfig.custom;
        ImGui::SeparatorText("Custom endpoint");
        ImGui::SetNextItemWidth(360 * s);
        ImGui::InputTextWithHint("Full URL", "optional: overrides Base URL + Path", &c.url);
        ImGui::SetNextItemWidth(260 * s);
        ImGui::InputTextWithHint("Path", c.format == ai::RequestFormat::AnthropicMessages ? "/v1/messages" : "/chat/completions", &c.path);
        const char* formats[] = {"OpenAI chat completions", "Anthropic messages"};
        int f = static_cast<int>(c.format);
        ImGui::SetNextItemWidth(260 * s);
        if (ImGui::Combo("Request format", &f, formats, 2)) c.format = static_cast<ai::RequestFormat>(f);
        const char* auths[] = {"Bearer (Authorization: Bearer <key>)", "x-api-key header", "Custom header", "None"};
        int a = static_cast<int>(c.auth);
        ImGui::SetNextItemWidth(260 * s);
        if (ImGui::Combo("Auth style", &a, auths, 4)) c.auth = static_cast<ai::AuthStyle>(a);
        if (c.auth == ai::AuthStyle::Header) {
            ImGui::SetNextItemWidth(260 * s);
            ImGui::InputTextWithHint("Auth header name", "e.g. api-key", &c.authHeaderName);
            ImGui::SetNextItemWidth(260 * s);
            ImGui::InputTextWithHint("Auth value prefix", "optional, e.g. \"Token \"", &c.authHeaderPrefix);
        }
        const char* mts[] = {"max_tokens", "max_completion_tokens", "none (omit)"};
        int m = static_cast<int>(c.maxTokensField);
        ImGui::SetNextItemWidth(260 * s);
        if (ImGui::Combo("Max-tokens field", &m, mts, 3)) c.maxTokensField = static_cast<ai::MaxTokensField>(m);
        ImGui::SetNextItemWidth(140 * s);
        ImGui::InputInt("Timeout (seconds)", &c.timeoutSeconds);
        c.timeoutSeconds = std::clamp(c.timeoutSeconds, 1, 600);
        ImGui::Checkbox("This model supports tool calling", &c.toolsEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off = the assistant chats only and sends no tool definitions. On = it may call the editor tools\n"
                              "(still limited by AI Control levels and permissions).");

        ImGui::TextDisabled("Extra headers (stored in the plain settings file: do NOT put keys here, use the auth style above):");
        int removeAt = -1;
        for (size_t i = 0; i < c.extraHeaders.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(150 * s);
            ImGui::InputTextWithHint("##hn", "Header-Name", &c.extraHeaders[i].name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200 * s);
            ImGui::InputTextWithHint("##hv", "value", &c.extraHeaders[i].value);
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_TRASH)) removeAt = static_cast<int>(i);
            ImGui::PopID();
        }
        if (removeAt >= 0) c.extraHeaders.erase(c.extraHeaders.begin() + removeAt);
        if (c.extraHeaders.size() < ai::CustomEndpoint::kMaxExtraHeaders && ImGui::SmallButton(ICON_PLUS " Add header")) c.extraHeaders.push_back({});

        if (!m_extraBodyTextInit) {
            m_extraBodyText = c.extraBody.is_object() && !c.extraBody.empty() ? c.extraBody.dump(2) : std::string();
            m_extraBodyError.clear();
            m_extraBodyTextInit = true;
        }
        ImGui::TextDisabled("Extra JSON body fields (merged into the request, e.g. {\"temperature\": 0.2}):");
        if (ImGui::InputTextMultiline("##extrabody", &m_extraBodyText, ImVec2(360 * s, 70 * s))) {
            m_extraBodyError.clear();
            if (m_extraBodyText.find_first_not_of(" \t\r\n") == std::string::npos) {
                c.extraBody = Json::object();
            } else {
                Json parsed = Json::parse(m_extraBodyText, nullptr, false);
                if (parsed.is_object()) c.extraBody = std::move(parsed);
                else m_extraBodyError = "Not a JSON object - the previous valid value is kept.";
            }
        }
        if (!m_extraBodyError.empty()) ImGui::TextColored(errCol, "%s %s", ICON_WARNING, m_extraBodyError.c_str());
    }

    // ---- what will be called
    {
        std::string url = m_aiConfig.requestUrl();
        if (size_t q = url.find('?'); q != std::string::npos) url = url.substr(0, q) + "?...";
        ImGui::TextDisabled("Requests go to: %s", url.empty() ? "(not set)" : url.c_str());
    }
    ai::EndpointCheck chk = ai::validateEndpoint(m_aiConfig);
    ImGui::PushTextWrapPos(0);
    for (auto& e : chk.errors) ImGui::TextColored(errCol, "%s %s", ICON_WARNING, e.c_str());
    for (auto& w : chk.warnings) ImGui::TextColored(warnCol, "%s %s", ICON_WARNING, w.c_str());
    if (!m_aiConfig.supportsTools())
        ImGui::TextColored(warnCol, "%s This model is not tool-capable. The assistant can chat, but it needs tool calling to use the editor tools "
                                    "(it will not be sent any tools).", ICON_INFO);
    ImGui::PopTextWrapPos();

    // ---- API key (credential store only)
    ImGui::SeparatorText("API key");
    std::string credId = m_aiConfig.resolvedCredentialId();
    bool has = m_keySource != ai::KeySource::None;
    if (!m_aiConfig.needsKey()) {
        ImGui::TextDisabled("%s No key needed for this endpoint (local server / auth style 'none').", ICON_INFO);
    } else {
        ImGui::TextColored(has ? okCol : ImVec4(1, 0.6f, 0.3f, 1), "%s %s", has ? ICON_LOCK : ICON_WARNING,
                           m_keySource == ai::KeySource::Store         ? "Key stored in the credential store"
                           : m_keySource == ai::KeySource::Environment ? "Key taken from an environment variable"
                                                                       : "No key configured");
    }
    ImGui::TextDisabled("Credential id '%s'. Store: %s. Env var: %s.", credId.c_str(), m_creds->backendName().c_str(),
                        ai::envVarForProvider(credId).c_str());
    ImGui::TextDisabled("The key value is never displayed, logged, or written to project / settings files.");
    ImGui::SetNextItemWidth(260 * s);
    ImGui::InputTextWithHint("##key", "paste API key to store it", &m_keyInput, ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button(ICON_LOCK " Store") && !m_keyInput.empty()) {
        auto st = m_creds->set(credId, m_keyInput);
        toast(st ? ToastLevel::Success : ToastLevel::Error, st ? "API key stored" : st.error);
        m_keyInput.assign(m_keyInput.size(), '\0');
        m_keyInput.clear();
        rebuildProvider();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_TRASH " Remove")) {
        (void)m_creds->remove(credId);
        rebuildProvider();
    }

    // ---- apply / test
    ImGui::Separator();
    if (ImGui::Button(ICON_CHECK "  Apply provider settings")) {
        saveSettings();
        rebuildProvider();
        toast(ToastLevel::Info, m_provider ? "AI provider: " + m_provider->name() : "AI provider not ready (missing key or invalid endpoint?)");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_connBusy.load() || !chk.ok());
    if (ImGui::Button(ICON_PLUG "  Test connection")) startTestConnection();  // reads the form as it is now (no Apply needed)
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Sends one tiny request (no tools, 16 tokens) to the endpoint above and reports status and latency.\n"
                          "It uses your key and counts as one billable request. The key is never shown.");
    ImGui::EndDisabled();  // m_aiBusy
    if (m_connBusy) {
        ImGui::SameLine();
        ImGui::TextDisabled("Testing...");
    } else {
        std::lock_guard l(m_connMutex);
        if (m_connHasResult) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(m_connResult.ok ? okCol : errCol, "%s %s", m_connResult.ok ? ICON_CHECK : ICON_WARNING, m_connResult.message.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    if (edited) {
        saveSettings();
        rebuildProvider();
    }
}

} // namespace fledge::editor
