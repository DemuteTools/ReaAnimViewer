@echo off
setlocal EnableExtensions
title ReaAnimViewer - Toolkit update test

rem ============================================================================
rem  Tests the self-update of a Demute Reaper Toolkit install without
rem  publishing anything (no GitHub Release, no ReaPack).
rem
rem  1 = Prepare: builds a release "new" version, waits for REAPER to run with
rem      the installed version, then puts the new one into the Toolkit folder, as
rem      a click on Update in the Toolkit would. Backs up the Toolkit folder first.
rem      The installed version is the real Toolkit install (the last release):
rem      the update is done by THAT version's self-update, as for real users.
rem  2 = Check: tells whether the installed DLL and video FX are the new version.
rem  3 = Restore: puts the Toolkit folder back, optionally reinstalls the dev build.
rem  4 = Diagnose: prints what the self-update looks at (to paste to a dev).
rem
rem  Needs: the ReaAnimViewer card installed from the Toolkit.
rem  The self-update skips a DLL that ReaPack owns: do not test on a PC where
rem  ReaAnimViewer is installed through ReaPack.
rem ============================================================================

cd /d "%~dp0"

rem A variable used by del / rmdir is always checked first: an empty one would
rem target the current folder (2026-10-03: an empty %DIAG% wiped this folder).

set "NEW_VER=0.2.99"
set "TOOLKIT=%APPDATA%\REAPER\Scripts\ReaAnimViewer\Scripts"
set "USERPLUGINS=%APPDATA%\REAPER\UserPlugins"
set "BACKUP=%TEMP%\RAV_update_test_backup"
set "STAGE=%TEMP%\RAV_update_test_new"

echo ============================================================
echo   ReaAnimViewer - Toolkit update test
echo ============================================================
echo   1 = Prepare  (builds %NEW_VER%, then simulates a Toolkit update)
echo   2 = Check    (after REAPER was quit and restarted)
echo   3 = Restore  (REAPER closed; puts the Toolkit folder back)
echo   4 = Diagnose (shows the files and versions the update looks at)
echo.
choice /C 1234 /M "Your choice"
if errorlevel 4 goto diagnose
if errorlevel 3 goto restore
if errorlevel 2 goto check
goto prepare

rem ---------------------------------------------------------------- Prepare --
:prepare
if not exist "%TOOLKIT%\RAV_Launcher.lua" goto no_toolkit

if exist "%BACKUP%\" goto backup_done
echo Backing up the Toolkit folder to %BACKUP% ...
xcopy "%TOOLKIT%" "%BACKUP%\" /E /I /Y /Q >nul
if errorlevel 1 goto failed
:backup_done

echo.
echo [1/2] Building the NEW version %NEW_VER% (not installed) ...
set "RAV_RELEASE_BUILD=1"
set "RAV_VERSION=%NEW_VER%"
call build.bat release noinstall nopause
if errorlevel 1 goto failed
set "RAV_RELEASE_BUILD="
set "RAV_VERSION="
if not defined STAGE goto failed
if exist "%STAGE%\" rmdir /S /Q "%STAGE%"
mkdir "%STAGE%"
copy /Y "build\Release\reaper_animviewer.dll" "%STAGE%\" >nul
if errorlevel 1 goto failed
copy /Y "build\Release\rav_video_fx.clap" "%STAGE%\" >nul
if errorlevel 1 goto failed

rem A real Toolkit update lands while REAPER is running, and the extension swaps
rem itself at quit. Files already there at startup would be swapped at the first
rem tick instead (the crash fallback, with a "restart" message): not the normal path.
echo.
echo ============================================================
echo   Start REAPER now (if it is not running) and open the viewer.
echo   Note the version shown in the viewer menu (the installed one).
echo   Leave REAPER open, come back here and press a key.
echo ============================================================
pause
tasklist /FI "IMAGENAME eq reaper.exe" 2>nul | find /I "reaper.exe" >nul
if errorlevel 1 goto reaper_not_running

echo.
echo [2/2] Putting %NEW_VER% in the Toolkit folder, as a click on Update would ...
copy /Y "%STAGE%\reaper_animviewer.dll" "%TOOLKIT%\" >nul
if errorlevel 1 goto failed
if not exist "%TOOLKIT%\FX\" mkdir "%TOOLKIT%\FX"
copy /Y "%STAGE%\rav_video_fx.clap" "%TOOLKIT%\FX\" >nul
if errorlevel 1 goto failed
if exist "%TOOLKIT%\rav_video_fx.clap" del /Q "%TOOLKIT%\rav_video_fx.clap"
rem The Toolkit copy's version is the @version of the launcher next to it.
powershell -NoProfile -Command "(Get-Content -Raw 'Scripts\RAV_Launcher.lua') -replace '(?m)^-- @version .*$', '-- @version %NEW_VER%' | Set-Content -NoNewline '%TOOLKIT%\RAV_Launcher.lua'"
if errorlevel 1 goto failed

echo.
echo ============================================================
echo   Update ready. Now:
echo   1. Quit REAPER (the update happens here, no message).
echo   2. Start REAPER again, open the viewer: the menu shows %NEW_VER%.
echo      Note any message, and whether the video FX is in the FX browser.
echo   3. Quit REAPER, run this file again and choose 2 = Check.
echo ============================================================
goto end

rem ------------------------------------------------------------------ Check --
:check
set "DLL_OK=0"
set "CLAP_OK=0"
powershell -NoProfile -Command "if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes('%USERPLUGINS%\reaper_animviewer.dll')).Contains('RAV_VERSION_MARKER:%NEW_VER%')) { exit 0 } else { exit 1 }"
if not errorlevel 1 set "DLL_OK=1"
powershell -NoProfile -Command "if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes('%USERPLUGINS%\FX\rav_video_fx.clap')).Contains('RAV_VERSION_MARKER:%NEW_VER%')) { exit 0 } else { exit 1 }"
if not errorlevel 1 set "CLAP_OK=1"

echo.
if "%DLL_OK%"=="1" echo   [OK]     Extension DLL is %NEW_VER%
if "%DLL_OK%"=="0" echo   [FAILED] Extension DLL is NOT %NEW_VER%
if "%CLAP_OK%"=="1" echo   [OK]     Video FX is %NEW_VER%
if "%CLAP_OK%"=="0" echo   [FAILED] Video FX is NOT %NEW_VER%
echo.
echo   When done, run this file again and choose 3 = Restore.
goto end

rem ---------------------------------------------------------------- Restore --
:restore
tasklist /FI "IMAGENAME eq reaper.exe" 2>nul | find /I "reaper.exe" >nul
if not errorlevel 1 goto reaper_running
if not exist "%BACKUP%\" goto no_backup
echo Restoring the Toolkit folder ...
if not defined TOOLKIT goto failed
if not defined BACKUP goto failed
rmdir /S /Q "%TOOLKIT%"
xcopy "%BACKUP%" "%TOOLKIT%\" /E /I /Y /Q >nul
if errorlevel 1 goto failed
rmdir /S /Q "%BACKUP%"
if exist "%STAGE%\" rmdir /S /Q "%STAGE%"
echo   Toolkit folder restored. The installed DLL stays %NEW_VER%.
goto ask_dev_build
:no_backup
echo   No backup to restore (Prepare was not run).
:ask_dev_build
echo.
choice /M "Reinstall your dev build (build.bat) now"
if errorlevel 2 goto end
set "RAV_RELEASE_BUILD="
set "RAV_VERSION="
call build.bat release nopause
if errorlevel 1 goto failed
echo.
echo   Dev build installed. Start REAPER as usual.
goto end

rem --------------------------------------------------------------- Diagnose --
:diagnose
set "DIAG=%TEMP%\RAV_update_diagnose.ps1"
> "%DIAG%" echo $ErrorActionPreference = 'SilentlyContinue'
>> "%DIAG%" echo function Ver($f) { $t = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)); $m = [regex]::Match($t, 'RAV_VERSION_MARKER:([^^\x00]{1,64})\x00'); if ($m.Success) { $m.Groups[1].Value } else { '(no version)' } }
>> "%DIAG%" echo foreach ($d in @('%USERPLUGINS%', '%USERPLUGINS%\FX', '%TOOLKIT%', '%TOOLKIT%\FX')) {
>> "%DIAG%" echo   Write-Output ''; Write-Output "== $d"
>> "%DIAG%" echo   if (-not (Test-Path $d)) { Write-Output '   (folder missing)'; continue }
>> "%DIAG%" echo   Get-ChildItem $d -File ^| Where-Object { $_.Name -match 'animviewer^|rav_' } ^| ForEach-Object { $v = ''; if ($_.Name -match '\.(dll^|clap)') { $v = Ver $_.FullName }; Write-Output ('   {0,-34} {1,10} bytes  {2:yyyy-MM-dd HH:mm}  {3}' -f $_.Name, $_.Length, $_.LastWriteTime, $v) }
>> "%DIAG%" echo }
>> "%DIAG%" echo Write-Output ''; Write-Output '== Launcher @version in the Toolkit folder'
>> "%DIAG%" echo Select-String -Path '%TOOLKIT%\RAV_Launcher.lua' -Pattern '@version' ^| Select-Object -First 1 ^| ForEach-Object { '   ' + $_.Line }
>> "%DIAG%" echo Write-Output ''; Write-Output '== ReaPack registry mentions the extension'
>> "%DIAG%" echo $db = '%APPDATA%\REAPER\ReaPack\registry.db'
>> "%DIAG%" echo if (Test-Path $db) { $t = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($db)); '   ' + $t.Contains('reaper_animviewer.dll') } else { '   (no ReaPack registry)' }
powershell -NoProfile -ExecutionPolicy Bypass -File "%DIAG%"
if defined DIAG if exist "%DIAG%" del /Q "%DIAG%" >nul 2>nul
echo.
echo   Copy everything above (select with the mouse, Enter) and paste it to the dev.
goto end

rem ----------------------------------------------------------------- Errors --
:no_toolkit
echo.
echo [ERROR] No Toolkit install found in %TOOLKIT%
echo Install the ReaAnimViewer card from the Demute Reaper Toolkit first.
goto end

:reaper_running
echo.
echo [ERROR] REAPER is running. Close it, then run this file again.
goto end

:reaper_not_running
echo.
echo [ERROR] REAPER is not running: the test needs it open, as for a real update.
echo Run this file again and choose 1.
goto end

:failed
echo.
echo [FAILED] See the messages above.
goto end

:end
echo.
pause
exit /b 0
