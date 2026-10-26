Unicode true
ManifestSupportedOS all
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

Name "loophole"
OutFile "${PACKAGE}\loophole-setup.exe"
InstallDir "$PROGRAMFILES64\loophole"
BrandingText "loophole ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "loophole"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "loophole guest installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "loophole contributors"

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\loophole"
!define VERB_KEY "Software\Classes\exefile\shell"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define TOOL '"$INSTDIR\loophole.exe"'

!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Failed

!macro Tool ARGS WHAT
  nsExec::Exec '${TOOL} --log "$INSTDIR\install.log" ${ARGS}'
  Pop $0
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 != 0
    StrCpy $Failed 1
    DetailPrint "${WHAT} failed (loophole.exe exit code $0)"
    MessageBox MB_ICONSTOP|MB_OK "${WHAT} failed (exit code $0). Details are in $INSTDIR\install.log." /SD IDOK
  ${EndIf}
!macroend

Section "Drivers" SecDrivers
  SectionIn RO
  SetOutPath "$INSTDIR"
  ${If} ${FileExists} "$INSTDIR\loophole-agent.exe"
    nsExec::Exec '${TOOL} agent stop'
    Pop $0
  ${EndIf}
  File "${TOOLS}\loophole.exe"
  File "${TOOLS}\loophole-agent.exe"
  WriteRegStr HKLM "${RUN_KEY}" "loophole-agent" '"$INSTDIR\loophole-agent.exe"'
  SetOutPath "$INSTDIR\drivers"
  File /r "${PACKAGE}\drivers\*"
  !insertmacro Tool 'testsigning enable' "Enabling test signing"
  ${If} $Failed == 1
    Abort
  ${EndIf}
  !insertmacro Tool 'driver setup "$INSTDIR\drivers"' "Installing the GPU driver"
  ${If} $Failed == 1
    Abort
  ${EndIf}
  SetRebootFlag true
SectionEnd

Section "Direct3D and OpenGL translation layers" SecLayers
  SetOutPath "$INSTDIR\layers"
  File /r "${PACKAGE}\layers\*"
  WriteRegStr HKLM "${VERB_KEY}\loophole.enable" "MUIVerb" "Use loophole for Direct3D and OpenGL"
  WriteRegStr HKLM "${VERB_KEY}\loophole.enable" "Icon" "$INSTDIR\loophole.exe"
  WriteRegStr HKLM "${VERB_KEY}\loophole.enable\command" "" '${TOOL} layers enable "%1" --notify'
  WriteRegStr HKLM "${VERB_KEY}\loophole.disable" "MUIVerb" "Stop using loophole for Direct3D and OpenGL"
  WriteRegStr HKLM "${VERB_KEY}\loophole.disable" "Icon" "$INSTDIR\loophole.exe"
  WriteRegStr HKLM "${VERB_KEY}\loophole.disable\command" "" '${TOOL} layers disable "%1" --notify'
  WriteRegStr HKLM "${VERB_KEY}\loophole.disable" "Extended" ""
SectionEnd

Section "-post"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "loophole"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "loophole"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\loophole.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" $0
  ${If} $Failed == 1
    SetErrorLevel 1
  ${ElseIf} ${RebootFlag}
    SetErrorLevel 3010
  ${EndIf}
SectionEnd

LangString DESC_Drivers ${LANG_ENGLISH} "The driver for the virtual GPU (display, Vulkan, Direct3D 10-12). Enables test-signing mode."
LangString DESC_Layers ${LANG_ENGLISH} "DXVK, vkd3d-proton and Zink for Direct3D 8/9 and OpenGL games. Right-click a game's .exe to use them for that game."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDrivers} $(DESC_Drivers)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecLayers} $(DESC_Layers)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP|MB_OK "loophole needs 64-bit Windows." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
  SetRegView 64
  StrCpy $Failed 0
FunctionEnd

Section "Uninstall"
  SetRegView 64
  nsExec::ExecToLog '${TOOL} agent stop'
  DeleteRegValue HKLM "${RUN_KEY}" "loophole-agent"
  nsExec::ExecToLog '${TOOL} layers disable-all'
  nsExec::ExecToLog '${TOOL} driver remove viogpu3d.inf'
  DeleteRegKey HKLM "${VERB_KEY}\loophole.enable"
  DeleteRegKey HKLM "${VERB_KEY}\loophole.disable"
  RMDir /r "$INSTDIR\drivers"
  RMDir /r "$INSTDIR\layers"
  Delete "$INSTDIR\loophole.exe"
  Delete "$INSTDIR\loophole-agent.exe"
  Delete "$INSTDIR\install.log"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  SetRebootFlag true
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd
