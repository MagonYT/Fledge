; Fledge installer (NSIS 3). Per-user install by default: no administrator rights needed.
;
; Built by:  cmake --build <build-dir> --target installer
;   -> stages `cmake --install` into <build-dir>/_installer/stage and runs a pinned,
;      SHA256-verified NSIS 3.10 (downloaded into <build-dir>/_tools, nothing installed system-wide).
; Manual: makensis /DVERSION=27.1.3 /DSTAGE=<staged install> /DOUTFILE=<setup.exe> installer\fledge.nsi
;
; This installer ships Fledge Standard only.
;
; Command line (setup.exe):
;   /S            silent install
;   /D=<dir>      install folder (must be the LAST argument, no quotes)
;   /TESTMODE     files only: no registry entries, Start Menu / desktop shortcuts or file
;                 association (used by tests/scripts/installer_test.py with a temp folder)
;   /LINKHANDLER  pre-checks "Register the fledge:// link handler" (off by default; ignored with /TESTMODE)
; Uninstall.exe /S removes the files, shortcuts and the per-user registry entries it created, removes the fledge://
; handler when it points at this install and restores any store link handlers Fledge took over
; (Fledge --link-handlers uninstall).

Unicode true
!ifndef VERSION
  !error "pass /DVERSION=x.y.z"
!endif
!ifndef STAGE
  !error "pass /DSTAGE=<staged cmake --install tree>"
!endif
!ifndef OUTFILE
  !define OUTFILE "Fledge-${VERSION}-setup.exe"
!endif
!ifndef SRCDIR
  !define SRCDIR "${STAGE}"
!endif

!ifndef EDITION
  !define EDITION "Standard"
!endif
!if "${EDITION}" != "Standard"
  !error "This source distribution supports only Fledge Standard"
!endif
!define PRODUCT "Fledge"
!define PRODUCT_LONG "Fledge Engine"
!define EDITION_ID "standard"
!define PRODUCT_KEY "Fledge"
!define UNINST_NAME "Fledge"
!define PROGID "Fledge.Project"
!define PUBLISHER "Fledge Project"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINST_NAME}"

Name "${PRODUCT_LONG} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT}"
InstallDirRegKey HKCU "Software\${PRODUCT_KEY}" "InstallDir"
SetCompressor /SOLID lzma
BrandingText "${PRODUCT_LONG} ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT_LONG}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${PRODUCT} ${VERSION} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "See LICENSE and THIRD_PARTY_LICENSES.md"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "StrFunc.nsh"
${UnStrLoc}

!define MUI_ICON "${STAGE}\resources\icons\fledge.ico"
!define MUI_UNICON "${STAGE}\resources\icons\fledge.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEFINISHPAGE_BITMAP "${__FILEDIR__}\wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${__FILEDIR__}\wizard.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "${__FILEDIR__}\header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "${__FILEDIR__}\header.bmp"
!define MUI_WELCOMEPAGE_TEXT "This installs ${PRODUCT_LONG} ${VERSION} for the current user (no administrator rights needed).$\r$\n$\r$\nIncludes the editor, the runtime, the fledgepack packager, the MCP server, project templates and licenses."
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\Fledge.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start the ${PRODUCT} Editor"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\THIRD_PARTY_LICENSES.md"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
Page custom LinkPageCreate LinkPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var TestMode
Var LinkHandler
Var LinkCheckbox

Function .onInit
  StrCpy $TestMode 0
  StrCpy $LinkHandler 0
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/TESTMODE" $R1
  ${IfNot} ${Errors}
    StrCpy $TestMode 1
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/LINKHANDLER" $R1
  ${IfNot} ${Errors}
    StrCpy $LinkHandler 1
  ${EndIf}
FunctionEnd

; Optional page: the per-user fledge:// link handler, unchecked by default. Unity Asset Store and Epic Games Launcher
; links are NOT touched here: taking those over is a separate opt-in inside Fledge, behind its own consent prompt.
Function LinkPageCreate
  !insertmacro MUI_HEADER_TEXT "Browser links (optional)" "Send assets from fab.com and the Unity Asset Store to Fledge"
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 72u "Registers the fledge:// link handler for your Windows account only (HKEY_CURRENT_USER), so the Send to Fledge browser extension and fledge:// links can hand store downloads to the Fledge editor.$\r$\n$\r$\nIt does not change how Unity Asset Store or Epic Games Launcher links open. The uninstaller removes it. You can also turn it on later in Fledge: Settings > Asset Store Import."
  Pop $0
  ${NSD_CreateCheckbox} 0 80u 100% 12u "Register the fledge:// link handler for my account"
  Pop $LinkCheckbox
  ${If} $LinkHandler == 1
    ${NSD_Check} $LinkCheckbox
  ${EndIf}
  nsDialogs::Show
FunctionEnd

Function LinkPageLeave
  ${NSD_GetState} $LinkCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $LinkHandler 1
  ${Else}
    StrCpy $LinkHandler 0
  ${EndIf}
FunctionEnd

; Refuse folders that already hold something other than an install of THIS edition, so the
; uninstaller (which removes bin\, templates\, ...) can never touch unrelated files or the other edition.
; .fledge_edition holds the edition id of an install (installs before 1.2.0 have none: they are Fledge).
; Result in $R9: 1 = acceptable, 0 = foreign non-empty folder (or the other edition's install).
!macro CheckInstDir
  StrCpy $R9 1
  ${If} ${FileExists} "$INSTDIR\bin\Fledge.exe"
    StrCpy $R8 "standard"
    ${If} ${FileExists} "$INSTDIR\.fledge_edition"
      FileOpen $R7 "$INSTDIR\.fledge_edition" r
      FileRead $R7 $R8
      FileClose $R7
    ${EndIf}
    ${If} $R8 != "${EDITION_ID}"
      StrCpy $R9 0
    ${EndIf}
  ${ElseIf} ${FileExists} "$INSTDIR\*.*"
    FindFirst $R0 $R1 "$INSTDIR\*.*"
    ${DoWhile} $R1 != ""
      ${If} $R1 != "."
      ${AndIf} $R1 != ".."
        StrCpy $R9 0
      ${EndIf}
      FindNext $R0 $R1
    ${Loop}
    FindClose $R0
  ${EndIf}
!macroend

Function .onVerifyInstDir
  !insertmacro CheckInstDir
  ${If} $R9 == 0
    Abort
  ${EndIf}
FunctionEnd

; Silent installs skip the directory page, so check /D=... again before copying anything.
Function CheckInstDirOrQuit
  !insertmacro CheckInstDir
  ${If} $R9 == 0
    MessageBox MB_ICONSTOP "$INSTDIR is not empty and does not contain a ${PRODUCT} installation (the other Fledge edition installs into its own folder). Choose an empty folder." /SD IDOK
    SetErrorLevel 2
    Quit
  ${EndIf}
FunctionEnd

Section "Fledge editor, runtime and tools (required)" SecCore
  SectionIn RO
  Call CheckInstDirOrQuit
  SetOutPath "$INSTDIR"
  ; portable.txt belongs to the ZIP only: installed copies keep user data in %APPDATA%\Fledge
  File /r /x portable.txt /x UserData "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  FileOpen $0 "$INSTDIR\.fledge_edition" w
  FileWrite $0 "${EDITION_ID}"
  FileClose $0
  ${If} $TestMode == 1
    FileOpen $0 "$INSTDIR\.fledge_test_install" w
    FileWrite $0 "Installed with /TESTMODE: no registry entries or shortcuts were created."
    FileClose $0
  ${Else}
    WriteRegStr HKCU "Software\${PRODUCT_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${PRODUCT_LONG}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\Fledge.exe,0"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
  ${EndIf}
SectionEnd

Section "Start Menu shortcuts" SecStartMenu
  ${If} $TestMode == 0
    CreateDirectory "$SMPROGRAMS\${PRODUCT}"
    CreateShortcut "$SMPROGRAMS\${PRODUCT}\${PRODUCT} Editor.lnk" "$INSTDIR\bin\Fledge.exe" "" "$INSTDIR\bin\Fledge.exe" 0
    CreateShortcut "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk" "$INSTDIR\Uninstall.exe"
  ${EndIf}
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  ${If} $TestMode == 0
    CreateShortcut "$DESKTOP\${PRODUCT} Editor.lnk" "$INSTDIR\bin\Fledge.exe" "" "$INSTDIR\bin\Fledge.exe" 0
  ${EndIf}
SectionEnd

Section "Open .fledge files with the ${PRODUCT} Editor" SecAssoc
  ${If} $TestMode == 0
    WriteRegStr HKCU "Software\Classes\.fledge" "" "${PROGID}"
    WriteRegStr HKCU "Software\Classes\.fledge\OpenWithProgids" "${PROGID}" ""
    WriteRegStr HKCU "Software\${PRODUCT_KEY}" "FileAssociation" "1"
    WriteRegStr HKCU "Software\Classes\${PROGID}" "" "${PRODUCT} Project"
    WriteRegStr HKCU "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\resources\icons\fledge_project.ico"
    WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\bin\Fledge.exe" "%1"'
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'  ; SHCNE_ASSOCCHANGED
  ${EndIf}
SectionEnd

Section "-fledge link handler (optional page)" SecLink
  ${If} $TestMode == 0
  ${AndIf} $LinkHandler == 1
    WriteRegStr HKCU "Software\Classes\fledge" "" "URL:Fledge import link"
    WriteRegStr HKCU "Software\Classes\fledge" "URL Protocol" ""
    WriteRegStr HKCU "Software\Classes\fledge\DefaultIcon" "" '"$INSTDIR\bin\Fledge.exe",0'
    WriteRegStr HKCU "Software\Classes\fledge\shell\open\command" "" '"$INSTDIR\bin\Fledge.exe" --open-url "%1"'
    WriteRegStr HKCU "Software\${PRODUCT_KEY}" "LinkHandler" "1"
  ${EndIf}
SectionEnd

LangString DESC_Core ${LANG_ENGLISH} "${PRODUCT}: editor, runtime, fledgepack, MCP server, templates and licenses."
LangString DESC_Start ${LANG_ENGLISH} "Start Menu folder with the editor and uninstaller."
LangString DESC_Desktop ${LANG_ENGLISH} "A ${PRODUCT} Editor shortcut on the desktop."
LangString DESC_Assoc ${LANG_ENGLISH} "Double-click a .fledge file to open it in ${PRODUCT} (per-user association)."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} $(DESC_Core)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} $(DESC_Start)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_Desktop)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc} $(DESC_Assoc)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  StrCpy $TestMode 0
  ${If} ${FileExists} "$INSTDIR\.fledge_test_install"
    StrCpy $TestMode 1
  ${EndIf}
  ; Link handlers first, while Fledge.exe still exists: restores any Unity / Epic link handlers Fledge took over (from
  ; the backup in HKCU\Software\Fledge\LinkHandlers) and removes fledge:// when it points at this install.
  ${If} $TestMode == 0
  ${AndIf} ${FileExists} "$INSTDIR\bin\Fledge.exe"
    ExecWait '"$INSTDIR\bin\Fledge.exe" --link-handlers uninstall'
  ${EndIf}
  ; Only what the installer created. User projects and %APPDATA%\${PRODUCT_KEY} settings are kept.
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\tools"
  RMDir /r "$INSTDIR\templates"
  RMDir /r "$INSTDIR\samples"
  RMDir /r "$INSTDIR\docs"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\resources"
  RMDir /r "$INSTDIR\sdk"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD_PARTY_LICENSES.md"
  Delete "$INSTDIR\.fledge_test_install"
  Delete "$INSTDIR\.fledge_edition"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  ${If} $TestMode == 0
    Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT} Editor.lnk"
    Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT} Documentation.lnk"
    Delete "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk"
    RMDir "$SMPROGRAMS\${PRODUCT}"
    Delete "$DESKTOP\${PRODUCT} Editor.lnk"
    ; Remove only our ProgID and clear the default only when it still points to Fledge.
    DeleteRegValue HKCU "Software\Classes\.fledge\OpenWithProgids" "${PROGID}"
    ReadRegStr $0 HKCU "Software\Classes\.fledge" ""
    ${If} $0 == "${PROGID}"
      DeleteRegValue HKCU "Software\Classes\.fledge" ""
    ${EndIf}
    DeleteRegKey /ifempty HKCU "Software\Classes\.fledge\OpenWithProgids"
    DeleteRegKey /ifempty HKCU "Software\Classes\.fledge"
    DeleteRegKey HKCU "Software\Classes\${PROGID}"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ; fallback when Fledge.exe could not run: remove fledge:// only when it is this install's
    ReadRegStr $0 HKCU "Software\Classes\fledge\shell\open\command" ""
    ${UnStrLoc} $1 $0 "$INSTDIR\bin\Fledge.exe" ">"
    ${If} $1 != ""
      DeleteRegKey HKCU "Software\Classes\fledge"
    ${EndIf}
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    DeleteRegKey HKCU "${UNINST_KEY}"
    ; a link-handler backup that could not be restored is kept (only an empty backup key is removed)
    DeleteRegKey /ifempty HKCU "Software\${PRODUCT_KEY}\LinkHandlers"
    DeleteRegValue HKCU "Software\${PRODUCT_KEY}" "InstallDir"
    DeleteRegValue HKCU "Software\${PRODUCT_KEY}" "LinkHandler"
    DeleteRegValue HKCU "Software\${PRODUCT_KEY}" "FileAssociation"
    DeleteRegKey /ifempty HKCU "Software\${PRODUCT_KEY}"
  ${EndIf}
SectionEnd
