@echo off
setlocal EnableExtensions
title ReaAnimViewer - ReaPack check

rem ============================================================================
rem  Validates the ReaPack package (Scripts\RAV_Launcher.lua, which also
rem  provides the extension DLL) with
rem  reapack-index --check, including uncommitted changes.
rem  The same check also runs on GitHub Actions at every push.
rem  First run on a PC: installs Ruby, Pandoc and reapack-index (several minutes).
rem ============================================================================

cd /d "%~dp0"

set "RUBY_BIN=C:\Ruby33-x64\bin"
if exist "%RUBY_BIN%\ruby.exe" set "PATH=%RUBY_BIN%;%PATH%"
if exist "%LOCALAPPDATA%\Pandoc\pandoc.exe" set "PATH=%LOCALAPPDATA%\Pandoc;%PATH%"

where reapack-index >nul 2>nul
if not errorlevel 1 goto run_check

echo reapack-index is not installed on this PC.
echo It needs Ruby (with its MSYS2 toolchain), Pandoc and CMake. One-time setup.
choice /M "Install them now"
if errorlevel 2 goto failed

where winget >nul 2>nul
if errorlevel 1 (
    echo [ERROR] winget is not available. Follow the manual steps on
    echo https://github.com/cfillion/reapack-index/wiki#installation
    goto failed
)
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake is required to build reapack-index. Install it from https://cmake.org/download/
    goto failed
)

where ruby >nul 2>nul
if errorlevel 1 (
    echo.
    echo [1/4] Installing Ruby with DevKit...
    winget install --id RubyInstallerTeam.RubyWithDevKit.3.3 -e --source winget --accept-source-agreements --accept-package-agreements
    set "PATH=%RUBY_BIN%;%PATH%"
)
where ruby >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Ruby was not found after installation. Open a new terminal and run this again.
    goto failed
)

echo.
echo [2/4] Installing the MSYS2 build toolchain for Ruby...
call ridk install 1 3

where pandoc >nul 2>nul
if errorlevel 1 (
    echo.
    echo [3/4] Installing Pandoc...
    winget install --id JohnMacFarlane.Pandoc -e --source winget --accept-source-agreements --accept-package-agreements
    set "PATH=%LOCALAPPDATA%\Pandoc;%PATH%"
)

echo.
echo [4/4] Installing reapack-index...
call gem install reapack-index
if errorlevel 1 (
    echo [ERROR] gem install reapack-index failed. See the messages above.
    goto failed
)

:run_check
echo.
echo Running reapack-index --check ...
echo.
call reapack-index --check
if errorlevel 1 (
    echo.
    echo [FAILED] The ReaPack package has errors. See the messages above.
    goto failed
)
echo.
echo OK: the ReaPack package is valid.
echo.
pause
exit /b 0

:failed
echo.
pause
exit /b 1
