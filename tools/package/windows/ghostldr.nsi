; ghostldr for Windows: a per-user installer, no administrator needed.
; Built by tools/package/build_windows.py, which passes VERSION, VERSION4, SRCDIR,
; OUTFILE, ICON and LISTS (files.nsh: what to replace on an update and remove
; on uninstall, so neither ever touches anything else in the folder).
;
; %LOCALAPPDATA%\ghostldr
;     python\          python.org's embeddable Python (amd64)
;     app\, ghostldr\  the release tree, as in the release zip
;     build\           what the scripts write (kept on an update)
;     ghostldr.ico, uninstall.exe
;
; Shortcuts run python\pythonw.exe app\launch.py: no console window.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "${LISTS}"

!define APPNAME "ghostldr"
!define HOMEPAGE "https://github.com/asylumexp/ghostldr"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\ghostldr"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\${APPNAME}"
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION4}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} ${VERSION} setup"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs ghostldr ${VERSION} for you only, in $LOCALAPPDATA\ghostldr, with its own Python: no administrator and nothing else needed.$\r$\n$\r$\nghostldr opens in your web browser. The first time it serves the game to the PS5, Windows Firewall may ask: allow Python on private networks.$\r$\n$\r$\nClick Next to continue."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start ghostldr"
!define MUI_FINISHPAGE_RUN_FUNCTION StartApp

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; A running copy holds its files: stop this folder's Pythons (only those).
!macro StopApp
  System::Call 'Kernel32::SetEnvironmentVariable(t "GHOSTLDR_PYTHON_DIR", t "$INSTDIR\python\")'
  nsExec::Exec `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Get-Process python,pythonw -ErrorAction SilentlyContinue | Where-Object { $$_.Path -and $$_.Path.StartsWith($$env:GHOSTLDR_PYTHON_DIR, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force"`
  Pop $0
!macroend

Section "ghostldr" SecApp
  SectionIn RO
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  !insertmacro StopApp
  ; an update replaces the old version's files; build\ stays
  !insertmacro RemoveRelease
  File /r "${SRCDIR}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\ghostldr.lnk" "$INSTDIR\python\pythonw.exe" '"$INSTDIR\app\launch.py"' \
    "$INSTDIR\ghostldr.ico" 0 SW_SHOWNORMAL "" "Prepare, install and play games on a PS5 through SWRR"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\ghostldr.ico"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKCU "${UNINST}" "URLInfoAbout" "${HOMEPAGE}"
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${UNINST}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetShellVarContext current
  CreateShortcut "$DESKTOP\ghostldr.lnk" "$INSTDIR\python\pythonw.exe" '"$INSTDIR\app\launch.py"' \
    "$INSTDIR\ghostldr.ico" 0 SW_SHOWNORMAL "" "Prepare, install and play games on a PS5 through SWRR"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "The app, with its own Python, and a Start menu shortcut."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function StartApp
  SetOutPath "$INSTDIR"
  Exec '"$INSTDIR\python\pythonw.exe" "$INSTDIR\app\launch.py"'
FunctionEnd

Section "Uninstall"
  SetShellVarContext current
  !insertmacro StopApp
  Delete "$SMPROGRAMS\ghostldr.lnk"
  Delete "$DESKTOP\ghostldr.lnk"
  !insertmacro RemoveRelease
  RMDir /r "$INSTDIR\build"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINST}"
SectionEnd
