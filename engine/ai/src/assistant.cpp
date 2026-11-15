#include "fledge/ai/assistant.h"

#include <format>

#include "fledge/core/redact.h"

namespace fledge::ai {

AssistantSession::AssistantSession(LlmProvider& provider, std::vector<ToolSpec> tools, ToolExecutor executor)
    : m_provider(provider), m_tools(std::move(tools)), m_exec(std::move(executor)) {}

std::string AssistantSession::defaultSystemPrompt() {
    return "You are the assistant built into the Fledge game engine editor. You can inspect and modify the open scene "
           "through the provided tools (entities, components, assets, play/stop, packaging). Use list_entities and "
           "describe_components to learn the current state before editing. Coordinates are meters, +Y is up, cameras "
           "look down -Z. Rotations can be given as [pitch, yaw, roll] degrees. For bigger requests (cities, vehicles, "
           "traffic, missions, day/night, sound) call list_capabilities first and prefer the high-level tools "
           "(generate_city_district, make_player, create_vehicle_from_preset, set_traffic, write_mission, populate_area); "
           "check your work with build_and_playtest and, if it reports issues, explain_failure. Every change is one undo "
           "step. list_capabilities also says which edition this is (Fledge or Fledge Advanced) and which features it has; if "
           "a tool answers requires_fledge_advanced, tell the user instead of working around it. Keep answers short and say what "
           "you changed.";
}

std::string AssistantSession::chatOnlySystemPrompt() {
    return "You are the assistant built into the Fledge game engine editor, but the selected model has no tool access in this "
           "session. You cannot inspect or change the scene. Answer questions about Fledge and game development, and when the "
           "user wants something built, describe the steps they can take in the editor. Never claim you changed anything.";
}

std::vector<ToolSpec> AssistantSession::toolSpecsFrom(const mcp::ToolRegistry& reg, const mcp::ToolPolicy& policy) {
    std::vector<ToolSpec> out;
    for (auto& t : reg.all())
        if (policy.permits(t)) out.push_back({t.name, t.description, t.inputSchema});
    return out;
}

static std::string toolResultText(const mcp::ToolResult& r) {
    std::string s;
    for (auto& block : r.content) {
        if (block.value("type", "") == "text") s += block.value("text", "");
        else if (block.value("type", "") == "image") s += "[image omitted]";
        s += "\n";
    }
    if (s.size() > 20000) s = s.substr(0, 20000) + "\n...(truncated)";
    return s;
}

Status AssistantSession::send(const std::string& userText, const std::function<void(const AssistantEvent&)>& onEvent) {
    auto emit = [&](AssistantEvent::Kind k, const std::string& t) {
        if (onEvent) onEvent({k, redact(t)});
    };
    m_history.push_back(ChatMessage::user(userText));
    int cutOff = 0;  // replies that ran into the output limit this turn
    for (int step = 0; step < m_maxSteps; ++step) {
        ChatRequest req;
        req.system = m_system;
        req.messages = m_history;
        req.tools = m_tools;
        // Agent-style providers (Claude Code) run their own tool loop and report what they do as it happens.
        req.onActivity = [&](const ProviderActivity& a) {
            if (!onEvent) return;
            AssistantEvent ev{AssistantEvent::Kind::AssistantText, redact(a.text)};
            switch (a.kind) {
                case ProviderActivity::Kind::Thinking: ev.kind = AssistantEvent::Kind::Thinking; break;
                case ProviderActivity::Kind::Text: break;
                case ProviderActivity::Kind::ToolCall:
                    ev.kind = AssistantEvent::Kind::ToolCall;
                    ev.toolName = a.toolName;
                    ev.toolArgs = Json::parse(redact(a.toolArgs.dump()), nullptr, false);
                    break;
                case ProviderActivity::Kind::ToolResult:
                    ev.kind = AssistantEvent::Kind::ToolResult;
                    ev.toolName = a.toolName;
                    ev.isError = a.isError;
                    break;
            }
            onEvent(ev);
        };
        auto resp = m_provider.chat(req);
        if (!resp) {
            emit(AssistantEvent::Kind::Error, resp.error());
            m_history.pop_back();  // keep history consistent (drop the unanswered user turn)
            return Status::failure(resp.error());
        }
        if (!resp->thinking.empty()) emit(AssistantEvent::Kind::Thinking, resp->thinking);
        if (resp->stopReason == "max_tokens") {
            // The model ran out of output room, usually in the middle of one very large tool call (a whole script file).
            // That reply is unusable: its tool call is incomplete. It used to end the turn without a word. Now the cut-off
            // reply is left out of the history and the model is asked to do the same work in smaller pieces.
            if (!resp->text.empty() && !resp->activityStreamed) emit(AssistantEvent::Kind::AssistantText, resp->text);
            if (++cutOff > 2) {
                emit(AssistantEvent::Kind::Error, "Stopped: the model hit its output limit three times in a row. Ask for the work in smaller parts.");
                return Status::failure("output limit reached repeatedly");
            }
            emit(AssistantEvent::Kind::Notice, "The reply was cut off at the output limit before it finished. Asking the model to redo it in smaller steps.");
            m_history.push_back(ChatMessage::user(
                "[Note from the Fledge editor] Your last reply reached the output limit and was discarded before it finished, so nothing "
                "from it was run. Do the same work in smaller steps: several small tool calls instead of one very large one, and keep "
                "any single file write to about 250 lines (write a long file in parts). Think briefly."));
            continue;
        }
        m_history.push_back(resp->asAssistantMessage());
        if (!resp->text.empty() && !resp->activityStreamed) emit(AssistantEvent::Kind::AssistantText, resp->text);
        if (resp->toolCalls.empty() || resp->stopReason == "refusal") {
            // Never end a turn in silence: say so when the model stopped without writing anything.
            if (resp->text.empty() && !resp->activityStreamed)
                emit(AssistantEvent::Kind::Notice, resp->stopReason.empty() || resp->stopReason == "end"
                                                       ? std::string("The model finished without writing a reply.")
                                                       : "The model stopped without a reply (reason: " + resp->stopReason + ").");
            emit(AssistantEvent::Kind::Done,
                 resp->cacheReadTokens || resp->cacheWriteTokens
                     ? std::format("tokens in/out: {}/{} (cache read {}, cache write {})", resp->inputTokens, resp->outputTokens,
                                   resp->cacheReadTokens, resp->cacheWriteTokens)
                     : std::format("tokens in/out: {}/{}", resp->inputTokens, resp->outputTokens));
            return {};
        }
        for (auto& call : resp->toolCalls) {
            if (onEvent) {
                AssistantEvent ev{AssistantEvent::Kind::ToolCall, redact(call.name + " " + call.arguments.dump())};
                ev.toolName = call.name;
                ev.toolArgs = Json::parse(redact(call.arguments.dump()), nullptr, false);
                onEvent(ev);
            }
            mcp::ToolResult r = m_exec(call.name, call.arguments);
            std::string text = toolResultText(r);
            if (onEvent) {
                AssistantEvent ev{AssistantEvent::Kind::ToolResult, redact((r.isError ? "error: " : "") + text)};
                ev.toolName = call.name;
                ev.isError = r.isError;
                onEvent(ev);
            }
            m_history.push_back(ChatMessage::toolResult(call.id, text, r.isError));
        }
    }
    emit(AssistantEvent::Kind::Error, "stopped after the maximum number of tool steps");
    return Status::failure("max steps reached");
}

} // namespace fledge::ai
