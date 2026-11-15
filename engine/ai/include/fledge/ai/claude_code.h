#pragma once
// Claude Code bridge: the AI Assistant can hand a turn to the user's own Claude Code CLI instead of calling an HTTP API.
//
// What runs: `claude -p --output-format stream-json ...` as a child process, with the prompt on stdin. Claude Code is the
// real program, signed in by the user themselves (`claude` then /login); the editor never sees, stores or forwards that
// sign-in, and sends no HTTP request of its own. Claude Code reaches the editor through the same MCP HTTP server every
// other MCP client uses, so tool permissions, AI control levels and the audit log apply unchanged.
//
// Built-in Claude Code tools that write files or run programs are not allowed from here: only the editor's MCP tools are
// pre-approved, and print mode refuses anything that would need a permission prompt.
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/ai/llm.h"

namespace fledge::ai {

struct ClaudeCodeOptions {
    std::string cliPath;        // "" = look on PATH and in the usual install folders
    std::string model;          // "" = Claude Code's own default / configured model
    std::string mcpUrl;         // the editor's MCP endpoint, e.g. http://127.0.0.1:8765/mcp ("" = chat only, no tools)
    std::string mcpToken;       // bearer token of that server ("" = none)
    fs::path workDir;           // working folder of the CLI (the project root)
    fs::path scratchDir;        // where the per-turn MCP config file is written (deleted after the turn)
    int timeoutSeconds = 900;   // one turn
    bool showThinking = true;
};

struct ClaudeCodeStatus {
    bool found = false;
    std::string path;     // resolved executable
    std::string message;  // what to tell the user when it was not found
};

// Resolves the CLI (explicit path, then PATH, then %USERPROFILE%\.local\bin and %APPDATA%\npm). Runs nothing.
ClaudeCodeStatus findClaudeCode(const std::string& cliPath);

// Runs a program with `stdinText` on its standard input and calls onLine for every output line (stdout and stderr).
// Returns the exit code, or -1 with `error` set when it could not start or timed out.
using ProcessRunner = std::function<int(const std::string& exe, const std::vector<std::string>& args, const fs::path& cwd,
                                        const std::string& stdinText, int timeoutSeconds,
                                        const std::function<void(std::string_view)>& onLine, std::string& error)>;
ProcessRunner defaultProcessRunner();

// The provider. `runner` is replaceable for tests (no real CLI needed).
std::unique_ptr<LlmProvider> createClaudeCodeProvider(ClaudeCodeOptions options, ProcessRunner runner = defaultProcessRunner());

// ---- pieces exposed for tests ---------------------------------------------------------------------------------------
// Arguments for one turn. sessionId "" = new conversation, else --resume. mcpConfig empty = no MCP server.
std::vector<std::string> claudeCodeArgs(const ClaudeCodeOptions& o, const std::string& sessionId, const fs::path& mcpConfig);
// The MCP config document handed to --mcp-config (server name "fledge").
Json claudeCodeMcpConfig(const ClaudeCodeOptions& o);
// Windows command line for CreateProcess; goes through cmd.exe for .cmd / .bat shims. Empty + error when an argument
// holds a character that cannot be passed safely.
std::string claudeCodeCommandLine(const std::string& exe, const std::vector<std::string>& args, std::string& error);
// Model ids and session ids are passed on a command line: letters, digits and . _ : - [ ] only.
bool claudeCodeSafeToken(const std::string& s);

struct ClaudeStreamState {
    std::string sessionId;                       // from the init / result events
    std::string finalText;                       // the result event's text
    std::string error;                           // result error, or the last non-JSON output line
    bool gotResult = false, isError = false;
    int inputTokens = 0, outputTokens = 0;
    std::map<std::string, std::string> toolNames;  // tool_use id -> display name
    std::string lastNoise;
};
// Feeds one stream-json line. Unknown or malformed lines never throw.
void handleClaudeStreamLine(std::string_view line, ClaudeStreamState& st, bool showThinking,
                            const std::function<void(const ProviderActivity&)>& onActivity);

} // namespace fledge::ai
