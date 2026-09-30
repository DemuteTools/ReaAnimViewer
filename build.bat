@echo off
setlocal EnableExtensions
title ReaAnimViewer - Build

rem ============================================================================
rem  ReaAnimViewer build script.
rem
rem  Usage:  build.bat [mode] [options]
rem
rem  Modes (pick one, default = release):
rem    release     Normal build, the one we ship (silent console).
rem    debuglog    Re-enables the [RAV] console logs (RAV_ENABLE_CONSOLE_LOG).
rem    forcefail   Forces the GL init failure path (RAV_FORCE_INIT_FAILURE).
rem
rem  Options:
rem    clean       Delete the build folder first (full rebuild).
rem    noinstall   Do not copy the DLL into REAPER's UserPlugins folder.
rem    nopause     Do not wait for a key press at the end (used by release.bat).
rem
rem  Double-click = release build + install into REAPER.
rem  The DLL reports version "dev": a dev build never updates itself from the
rem  Demute Reaper Toolkit copy. Only release.bat compiles a real version in
rem  (it sets RAV_RELEASE_BUILD=1 and RAV_VERSION for this run only).
rem  Examples:  build.bat debuglog
rem             build.bat release clean
rem ============================================================================

cd /d "%~dp0"

set "MODE=release"
set "CLEAN=0"
set "INSTALL=1"
set "PAUSE_AT_END=1"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="release"   set "MODE=release"   & shift & goto parse_args
if /I "%~1"=="debuglog"  set "MODE=debuglog"  & shift & goto parse_args
if /I "%~1"=="forcefail" set "MODE=forcefail" & shift & goto parse_args
if /I "%~1"=="clean"     set "CLEAN=1"        & shift & goto parse_args
if /I "%~1"=="noinstall" set "INSTALL=0"      & shift & goto parse_args
if /I "%~1"=="nopause"   set "PAUSE_AT_END=0" & shift & goto parse_args
echo [ERROR] Unknown argument: %~1
echo Usage: build.bat [release^|debuglog^|forcefail] [clean] [noinstall] [nopause]
set "RESULT=1"
goto done
:args_done

if /I "%MODE%"=="release"   set "BUILD_DIR=build"           & set "DEFINE="
if /I "%MODE%"=="debuglog"  set "BUILD_DIR=build-debuglog"  & set "DEFINE=RAV_ENABLE_CONSOLE_LOG"
if /I "%MODE%"=="forcefail" set "BUILD_DIR=build-forcefail" & set "DEFINE=RAV_FORCE_INIT_FAILURE"

set "DLL=%BUILD_DIR%\Release\reaper_animviewer.dll"

rem A stray RAV_VERSION in the user's environment must not turn a dev build into
rem a "release" one (see the note at the top).
set "VERSION_ARG="
if "%RAV_RELEASE_BUILD%"=="1" set "VERSION_ARG=%RAV_VERSION%"
set "USERPLUGINS=%APPDATA%\REAPER\UserPlugins"

echo ============================================================
echo   ReaAnimViewer build  -  mode: %MODE%
if not "%DEFINE%"=="" echo   Test build with %DEFINE%. Do NOT ship this DLL.
echo ============================================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake was not found in PATH.
    echo Install it from https://cmake.org/download/ and tick
    echo "Add CMake to the system PATH", then run this script again.
    set "RESULT=1"
    goto done
)

if "%CLEAN%"=="1" if exist "%BUILD_DIR%\" (
    echo [clean] Deleting %BUILD_DIR%\ ...
    rmdir /S /Q "%BUILD_DIR%"
)

echo [1/3] Configuring...
if "%DEFINE%"=="" (
    cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 "-DRAV_VERSION=%VERSION_ARG%"
) else (
    cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 "-DRAV_VERSION=%VERSION_ARG%" -DCMAKE_CXX_FLAGS="/D %DEFINE%"
)
if errorlevel 1 (
    echo.
    echo [FAILED] CMake configuration failed. See the messages above.
    set "RESULT=1"
    goto done
)

echo.
echo [2/3] Compiling (a few minutes on the first build)...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo.
    echo [FAILED] Compilation failed. Look for the first line containing "error" above.
    set "RESULT=1"
    goto done
)

if not exist "%DLL%" (
    echo [FAILED] Build finished but %DLL% is missing.
    set "RESULT=1"
    goto done
)

echo.
if "%INSTALL%"=="0" (
    echo [3/3] Install skipped ^(noinstall^).
    goto success
)

echo [3/3] Installing into REAPER...
tasklist /FI "IMAGENAME eq reaper.exe" 2>nul | find /I "reaper.exe" >nul
if not errorlevel 1 (
    echo [WARNING] REAPER is running: the DLL is locked and cannot be replaced.
    echo Close REAPER, then press a key to retry the copy.
    pause >nul
)
if not exist "%USERPLUGINS%\" mkdir "%USERPLUGINS%"
copy /Y "%DLL%" "%USERPLUGINS%\" >nul
if errorlevel 1 (
    echo [FAILED] Could not copy the DLL. Make sure REAPER is closed and run again.
    set "RESULT=1"
    goto done
)
echo Installed to %USERPLUGINS%

:success
echo.
echo ============================================================
echo   OK  -  %DLL%
if "%INSTALL%"=="1" echo   Start REAPER and run the action "RAV: Open Viewer".
if /I "%MODE%"=="debuglog"  echo   Open the REAPER console: [RAV] log lines should appear.
if /I "%MODE%"=="forcefail" echo   Opening the viewer should show the GL init error message.
if not "%DEFINE%"=="" echo   When done testing, run build.bat again to reinstall the normal build.
echo ============================================================
set "RESULT=0"

:done
echo.
if "%PAUSE_AT_END%"=="1" pause
exit /b %RESULT%
