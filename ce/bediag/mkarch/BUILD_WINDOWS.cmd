@echo off
setlocal
set SCRIPT_DIR=%~dp0
set TOOL_DIR=%SCRIPT_DIR%..\..\..\be300 Installer\Installer
set SOURCE_DIR=%SCRIPT_DIR%windows_system
set OUT_DIR=%SCRIPT_DIR%out\windows_system

if not exist "%SOURCE_DIR%\BEDiag.dll" (
  echo ERROR: Copy WMIPSRel\BEDiag.dll to %SOURCE_DIR% before running this script.
  exit /b 1
)

if not exist "%SOURCE_DIR%\BEDiagKick.exe" (
  echo ERROR: Copy WMIPSRel\BEDiagKick.exe to %SOURCE_DIR% before running this script.
  exit /b 1
)

if not exist "%TOOL_DIR%\MkArch.exe" (
  echo ERROR: MkArch.exe not found at %TOOL_DIR%
  exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

"%TOOL_DIR%\MkArch.exe" "%SOURCE_DIR%" "%OUT_DIR%\BEDiag_windows.cbea" -p 0000 -o c000 -c 0000
if errorlevel 1 exit /b %errorlevel%

copy /y "%TOOL_DIR%\Setup.exe" "%OUT_DIR%\Setup.exe" >nul
copy /y "%SCRIPT_DIR%wrapper\BEDiag_windows.Setup.ini" "%OUT_DIR%\Setup.ini" >nul

echo Built package in %OUT_DIR%
