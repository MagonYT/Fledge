#pragma once
// Engine install layout and per-user data location. Nothing is hard-coded:
// every path is resolved relative to the running executable.
//
//   Install tree (cmake --install / portable ZIP / installer):
//     <root>/bin/Fledge.exe ...   <root>/templates  <root>/samples
//     <root>/docs  <root>/licenses  <root>/resources/{fonts,icons}  <root>/sdk/include (Fledge Script)
//   Build tree: <build>/bin/fledge_paths.json (generated at configure time) maps
//     each directory to its location relative to bin/ (sources, build/resources) and keeps
//     user data in <build>/UserData so development runs never write to %APPDATA%.
//
// Portable mode (portable.txt in <root> or next to the exe, or --portable):
// user data (settings, layouts, recent projects, default projects folder) lives
// in <root>/UserData instead of %APPDATA%\Fledge.
#include <string>

#include "fledge/core/utility.h"

namespace fledge {

struct EnginePaths {
    fs::path exeDir;
    fs::path root;       // install root (exeDir/.. for an install, the source tree for a build)
    fs::path resources;  // fonts, icons
    fs::path templates;  // project templates (New Project wizard)
    fs::path samples;
    fs::path docs;
    fs::path licenses;
    fs::path sdk;        // Fledge Script SDK include dir (<root>/sdk/include; engine/scripting/sdk in a build tree)
    fs::path userData;   // settings dir: %APPDATA%\Fledge, or <root>/UserData when portable
    bool portable = false;
    bool buildTree = false;  // resolved through fledge_paths.json

    std::string describe() const;  // multi-line "key: value" dump (Fledge --print-paths)
};

// Pure resolution (unit-testable). appDataDir: value of %APPDATA% (empty = unknown).
// userDataFolder: the folder under appDataDir ("Fledge"; "Fledge Advanced" for the Advanced editor and tools).
EnginePaths resolveEnginePaths(const fs::path& exeDir, bool forcePortable, const fs::path& appDataDir,
                               const std::string& userDataFolder = "Fledge");

// Process-wide paths, resolved on first use from executablePath() and %APPDATA%.
// The environment variable FLEDGE_USER_DATA, when set, replaces userData (tests / sandboxes) - except in
// portable mode, which always keeps user data inside the install.
// Call initEnginePaths() early (before first use) to apply --portable.
const EnginePaths& enginePaths();
void initEnginePaths(bool forcePortable);
// Folder under %APPDATA% used by initEnginePaths() (default "Fledge"). The editor and tools of Fledge Advanced call it
// with "Fledge Advanced" before the first enginePaths() use, so the two editions keep separate settings; packaged
// games never call it (their per-player data stays in %APPDATA%\Fledge\Games\<game>, whichever edition built them).
void setUserDataFolderName(const std::string& name);
// %APPDATA% on Windows (XDG_CONFIG_HOME or ~/.config elsewhere); empty when unknown. The folder resolveEnginePaths() puts
// "Fledge" / "Fledge Advanced" under (the settings migration uses it to find the other edition's folder).
fs::path platformAppDataDir();

// For GUI-subsystem executables: keeps printf output working.
//   * stdout/stderr redirected to a pipe or file (ctest, scripts): used as-is.
//   * started from a console window: attaches to the parent console.
//   * double-clicked: no console, output is discarded (logs still go to the log ring / panels).
void attachParentConsoleIfNeeded();

} // namespace fledge
