; FrameLift installer (NSIS 3). Per-user: no administrator prompt, installs to
; %LOCALAPPDATA%\Programs\FrameLift. Build with tools/make_installer.sh.
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!define APP "FrameLift"
!define EXE "FrameLift.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APP}"
OutFile "..\dist\${APP}-Setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License, FrameLift contributors"

!define MUI_ICON "..\res\framelift.ico"
!define MUI_UNICON "..\res\framelift.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install ${APP}"
!define MUI_WELCOMEPAGE_TEXT "${APP} plays your console through a capture card at a higher frame rate, with as little added latency as possible.$\r$\n$\r$\nIt works with Elgato, AVerMedia and most USB HDMI capture devices on Windows 10 and 11 with a DirectX 11 GPU. NVIDIA GTX 16-series / RTX cards use their optical flow hardware for the best quality.$\r$\n$\r$\nNo administrator rights are needed."
!define MUI_COMPONENTSPAGE_NODESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP} now"
!define MUI_FINISHPAGE_TEXT "Plug in your capture card and open ${APP}. Press F11 to play fullscreen.$\r$\n$\r$\nClose OBS, Elgato Studio or anything else using the capture card first: Windows lets only one app use it at a time."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APP} needs 64-bit Windows 10 or 11."
    Abort
  ${EndIf}
  ; Windows "N" editions ship without Media Foundation, which capture needs.
  ${IfNot} ${FileExists} "$SYSDIR\mfplat.dll"
    MessageBox MB_ICONEXCLAMATION|MB_OKCANCEL "This copy of Windows has no Media Foundation (Windows N edition). ${APP} needs it to read capture cards.$\r$\n$\r$\nInstall the free Media Feature Pack: Settings > Apps > Optional features > Add a feature > Media Feature Pack.$\r$\n$\r$\nPress OK to install ${APP} anyway." IDOK +2
    Abort
  ${EndIf}
FunctionEnd

Section "${APP}" SecMain
  SectionIn RO
  ; Close a running copy so its files can be replaced.
  nsExec::Exec 'taskkill /IM ${EXE}'
  Sleep 800
  nsExec::Exec 'taskkill /F /IM ${EXE}'
  SetOutPath "$INSTDIR"
  File "..\build-mingw\${EXE}"
  File "THIRD_PARTY_NOTICES.txt"
  File "/oname=LICENSE.txt" "..\LICENSE"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" 3000
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Section /o "Start with Windows (opens fullscreen)" SecAutostart
  WriteRegStr HKCU "${RUN_KEY}" "${APP}" '"$INSTDIR\${EXE}" --fullscreen'
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM ${EXE}'
  Sleep 800
  nsExec::Exec 'taskkill /F /IM ${EXE}'
  ; Remove the NVIDIA power-mode entry FrameLift added to its own driver profile.
  ExecWait '"$INSTDIR\${EXE}" --nv-reset'
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  DeleteRegValue HKCU "${RUN_KEY}" "${APP}"
  DeleteRegKey HKCU "${UNINST_KEY}"
  ; Settings and logs: keep by default (a reinstall picks them up).
  IfSilent done
  MessageBox MB_YESNO|MB_DEFBUTTON2 "Also delete ${APP} settings, logs and saved test clips?" IDNO done
    RMDir /r "$APPDATA\${APP}"
    RMDir /r "$LOCALAPPDATA\${APP}"
  done:
SectionEnd
