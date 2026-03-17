@echo off
setlocal
set SCRIPT_DIR=%~dp0
set TOOL_DIR=%SCRIPT_DIR%..
set OUT_DIR=%SCRIPT_DIR%out
set STAGE_DIR=%OUT_DIR%\stage

if not exist "%SCRIPT_DIR%BEDiag.dll" (
  echo ERROR: Copy WMIPSRel\BEDiag.dll to %SCRIPT_DIR% before running this script.
  exit /b 1
)

if not exist "%SCRIPT_DIR%BEDiagKick.exe" (
  echo ERROR: Copy WMIPSRel\BEDiagKick.exe to %SCRIPT_DIR% before running this script.
  exit /b 1
)

if not exist "%TOOL_DIR%\MkArch.exe" (
  echo ERROR: MkArch.exe not found at %TOOL_DIR%
  exit /b 1
)

if not exist "%TOOL_DIR%\Setup.exe" (
  echo ERROR: Setup.exe not found at %TOOL_DIR%
  exit /b 1
)

if not exist "%TOOL_DIR%\native.ina" (
  echo ERROR: native.ina not found at %TOOL_DIR%
  exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
mkdir "%STAGE_DIR%"

copy /y "%SCRIPT_DIR%Install.inf" "%STAGE_DIR%\Install.inf" >nul
copy /y "%SCRIPT_DIR%UnBEDiag.inf" "%STAGE_DIR%\UnBEDiag.inf" >nul
copy /y "%SCRIPT_DIR%apps.txt" "%STAGE_DIR%\apps.txt" >nul
copy /y "%SCRIPT_DIR%InsResetFlag.txt" "%STAGE_DIR%\InsResetFlag.txt" >nul
copy /y "%SCRIPT_DIR%BEDiag.dll" "%STAGE_DIR%\BEDiag.dll" >nul
copy /y "%SCRIPT_DIR%BEDiagKick.exe" "%STAGE_DIR%\BEDiagKick.exe" >nul
copy /y "%TOOL_DIR%\Setup.exe" "%STAGE_DIR%\Setup.exe" >nul
copy /y "%TOOL_DIR%\native.ina" "%STAGE_DIR%\native.ina" >nul

"%TOOL_DIR%\MkArch.exe" "%STAGE_DIR%" "%OUT_DIR%\BEDiag.cbea" -p 0000 -o c000 -c 0000
if errorlevel 1 exit /b %errorlevel%

copy /y "%STAGE_DIR%\Setup.exe" "%OUT_DIR%\Setup.exe" >nul
copy /y "%SCRIPT_DIR%Setup.ini" "%OUT_DIR%\Setup.ini" >nul

echo Built package in %OUT_DIR%
