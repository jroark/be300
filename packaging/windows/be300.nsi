; BE-300 emulator Windows installer.
;
; Builds dist/be300-setup.exe from build-windows/dist/be300-windows-amd64/.
; Wraps the existing tools/build_windows.sh output without duplicating its
; download-SDL-DLL logic.

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.1.1"
!endif

Name        "BE-300 Emulator"
OutFile     "..\..\dist\be300-setup.exe"
InstallDir  "$PROGRAMFILES64\BE300"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_ICON "be300.ico"
!define MUI_UNICON "be300.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "BE-300" SecCore
  SetOutPath "$INSTDIR"

  ; Files produced by tools/build_windows.sh in build-windows/dist/be300-windows-amd64/
  File "..\..\dist\be300-windows-amd64\be300.exe"
  File "..\..\dist\be300-windows-amd64\SDL2.dll"
  File /nonfatal "..\..\dist\be300-windows-amd64\run-be300.bat"
  File "be300.ico"

  ; Start Menu shortcut
  CreateDirectory "$SMPROGRAMS\BE-300"
  CreateShortCut  "$SMPROGRAMS\BE-300\BE-300 VM Manager.lnk" \
                  "$INSTDIR\be300.exe" "" "$INSTDIR\be300.ico"
  CreateShortCut  "$SMPROGRAMS\BE-300\Uninstall.lnk" \
                  "$INSTDIR\uninstall.exe"

  ; Register .be300vm file association so double-click opens the launcher
  ; with the bundle path as argv[1].
  WriteRegStr HKCR ".be300vm" "" "BE300.VirtualMachine"
  WriteRegStr HKCR "BE300.VirtualMachine" "" "BE-300 Virtual Machine"
  WriteRegStr HKCR "BE300.VirtualMachine\DefaultIcon" "" "$INSTDIR\be300.ico,0"
  WriteRegStr HKCR "BE300.VirtualMachine\shell\open\command" "" \
                   '"$INSTDIR\be300.exe" "%1"'

  ; Add/Remove Programs entry
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300" \
              "DisplayName" "BE-300 Emulator"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300" \
              "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300" \
              "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300" \
              "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300" \
              "DisplayIcon" "$INSTDIR\be300.ico"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300" \
              "Publisher" "BE-300 emulator project"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\be300.exe"
  Delete "$INSTDIR\SDL2.dll"
  Delete "$INSTDIR\run-be300.bat"
  Delete "$INSTDIR\be300.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir  "$INSTDIR"

  Delete "$SMPROGRAMS\BE-300\BE-300 VM Manager.lnk"
  Delete "$SMPROGRAMS\BE-300\Uninstall.lnk"
  RMDir  "$SMPROGRAMS\BE-300"

  DeleteRegKey HKCR ".be300vm"
  DeleteRegKey HKCR "BE300.VirtualMachine"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BE300"
SectionEnd
