@echo off
setlocal EnableExtensions
title ReaAnimViewer - Release

rem ============================================================================
rem  Publishes a new version of ReaAnimViewer.
rem  - asks for the version number and the changelog
rem  - clean release build + VC++ runtime check
rem  - bumps the version (Extensions\ReaAnimViewer.ext, CMakeLists.txt, README.md)
rem  - commit + tag + GitHub Release with the DLL
rem  - push: GitHub Actions regenerates index.xml with reapack-index
rem  GitHub CLI is installed automatically if missing. Logic: tools\release.ps1
rem ============================================================================

cd /d "%~dp0"

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git was not found in PATH.
    goto failed
)

call "%~dp0tools\ensure-gh.bat" || goto failed

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\release.ps1"
if errorlevel 1 goto failed

echo.
pause
exit /b 0

:failed
echo.
echo Release aborted.
pause
exit /b 1
