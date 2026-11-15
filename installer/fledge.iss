; Fledge installer - Inno Setup 6 script.
;
; STATUS: UNBUILT AND UNTESTED. Inno Setup is not available on the development
; machine; only the NSIS script (installer/fledge.nsi) is built and tested
; (tests/scripts/installer_test.py). This script mirrors the NSIS one
; (per-user install, Start Menu + optional desktop shortcut, .fledge
; association, uninstaller) and is kept as a starting point. NSIS is the real
; installer; 1.2.4, 1.3.0, 1.3.1, 1.3.2 and 1.3.3 only bumped the version here (no editions, no link handlers).
;
; Build (untested):
;   cmake --install build/ninja-gcc-release --prefix build/ninja-gcc-release/_installer/stage
;   iscc /DVERSION=27.1.3 /DSTAGE=build\ninja-gcc-release\_installer\stage installer\fledge.iss

#ifndef VERSION
  #define VERSION "27.1.3"
#endif
#ifndef STAGE
  #error Pass /DSTAGE=<staged cmake --install tree>
#endif

[Setup]
AppId={{6F1C4D2A-7C3B-4E55-9A2E-CA9E0B5E0001}
AppName=Fledge Engine
AppVersion={#VERSION}
AppPublisher=Fledge Project
AppVerName=Fledge {#VERSION}
DefaultDirName={localappdata}\Programs\Fledge
DefaultGroupName=Fledge
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=Fledge-{#VERSION}-setup-inno
SetupIconFile={#STAGE}\resources\icons\fledge.ico
UninstallDisplayIcon={app}\bin\Fledge.exe
LicenseFile={#STAGE}\THIRD_PARTY_LICENSES.md
Compression=lzma2
SolidCompression=yes
ChangesAssociations=yes
DisableProgramGroupPage=yes
VersionInfoVersion={#VERSION}.0
VersionInfoProductName=Fledge Engine

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "assoc"; Description: "Open .fledge files with the Fledge Editor"; GroupDescription: "File association:"
Name: "linkhandler"; Description: "Register the fledge:// link handler for my account (Send to Fledge)"; GroupDescription: "Browser links:"; Flags: unchecked

[Files]
; portable.txt belongs to the ZIP only
Source: "{#STAGE}\*"; DestDir: "{app}"; Excludes: "portable.txt,UserData"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Fledge Editor"; Filename: "{app}\bin\Fledge.exe"
Name: "{group}\Fledge Documentation"; Filename: "{app}\docs"
Name: "{group}\Uninstall Fledge"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Fledge Editor"; Filename: "{app}\bin\Fledge.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.fledge"; ValueType: string; ValueName: ""; ValueData: "Fledge.Project"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Fledge.Project"; ValueType: string; ValueName: ""; ValueData: "Fledge Project"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Fledge.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\resources\icons\fledge_project.ico"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Fledge.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\Fledge.exe"" ""%1"""; Tasks: assoc

Root: HKCU; Subkey: "Software\Classes\fledge"; ValueType: string; ValueName: ""; ValueData: "URL:Fledge import link"; Flags: uninsdeletekey; Tasks: linkhandler
Root: HKCU; Subkey: "Software\Classes\fledge"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: linkhandler
Root: HKCU; Subkey: "Software\Classes\fledge\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\Fledge.exe"" --open-url ""%1"""; Tasks: linkhandler

[Run]
Filename: "{app}\bin\Fledge.exe"; Description: "Start the Fledge Editor"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; restores Unity / Epic link handlers Fledge took over and removes its own fledge:// handler (untested, like this script)
Filename: "{app}\bin\Fledge.exe"; Parameters: "--link-handlers uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "FledgeLinkHandlers"
