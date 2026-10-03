@echo off
setlocal EnableExtensions
title ReaAnimViewer - Clean uninstall (test)

rem ============================================================================
rem  Removes every ReaAnimViewer install from this REAPER, whatever installed it
rem  (Demute Reaper Toolkit, ReaPack, build.bat, old versions, spikes), so the
rem  next install starts from a clean REAPER. Lists what it finds, with the
rem  version, before removing anything:
rem    - UserPlugins: the extension (reaper_animviewer.dll and the pre-rename
rem      reaper_fbxanimationviewer.dll / reaper_fbxav_spike.dll), .old / .new
rem    - UserPlugins\FX: the video FX (rav_video_fx.clap), the Epic 11 spike
rem      (rav_video_fx_spike.clap / .vst3), .old / .new
rem    - Scripts\ReaAnimViewer (Toolkit and ReaPack launcher) and the Toolkit's
rem      version cache
rem    - the launcher's entry in the Actions list (reaper-kb.ini, backed up first)
rem    - the leftovers of test-toolkit-update.bat in %TEMP%
rem  Keeps the viewer settings, toolbars and shortcuts.
rem  Does not edit ReaPack's registry: if ReaPack lists the extension, it says
rem  so, and you uninstall the package in ReaPack.
rem  REAPER must be closed.
rem ============================================================================

rem Never run from the repository folder: nothing here touches it.
cd /d "%TEMP%"

set "RES=%APPDATA%\REAPER"
set "USERPLUGINS=%RES%\UserPlugins"
set "FXDIR=%RES%\UserPlugins\FX"
set "SCRIPTDIR=%RES%\Scripts\ReaAnimViewer"

echo ============================================================
echo   ReaAnimViewer - clean uninstall
echo   REAPER folder: %RES%
echo ============================================================

if not exist "%RES%\" goto no_reaper
tasklist /FI "IMAGENAME eq reaper.exe" 2>nul | find /I "reaper.exe" >nul
if not errorlevel 1 goto reaper_running

rem ---- What is installed -----------------------------------------------------
set "LIST=%TEMP%\RAV_uninstall_list.ps1"
> "%LIST%" echo $ErrorActionPreference = 'SilentlyContinue'
>> "%LIST%" echo function Ver($f) { $t = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)); $m = [regex]::Match($t, 'RAV_VERSION_MARKER:([^^\x00]{1,64})\x00'); if ($m.Success) { $m.Groups[1].Value } else { '' } }
>> "%LIST%" echo foreach ($d in @('%USERPLUGINS%', '%FXDIR%', '%SCRIPTDIR%', '%SCRIPTDIR%\Scripts', '%SCRIPTDIR%\Scripts\FX')) {
>> "%LIST%" echo   if (-not (Test-Path $d)) { continue }
>> "%LIST%" echo   Get-ChildItem $d -File ^| Where-Object { $_.Name -match 'animviewer^|fbxav^|rav_^|RAV_' } ^| ForEach-Object { $v = ''; if ($_.Name -match '\.(dll^|clap^|vst3)') { $v = Ver $_.FullName } elseif ($_.Name -match '\.lua$') { $v = (Select-String -Path $_.FullName -Pattern '@version\s+(\S+)' ^| Select-Object -First 1).Matches.Groups[1].Value }; Write-Output ('   {0,-70} {1}' -f $_.FullName.Substring('%RES%'.Length + 1), $v) }
>> "%LIST%" echo }
>> "%LIST%" echo if (Test-Path '%RES%\Scripts\DM_ReaperToolkit\cache\ReaAnimViewer.xml') { Write-Output '   Scripts\DM_ReaperToolkit\cache\ReaAnimViewer.xml' }
echo.
echo Found (file, version):
powershell -NoProfile -ExecutionPolicy Bypass -File "%LIST%"
if defined LIST if exist "%LIST%" del /Q "%LIST%" >nul 2>nul
echo.

choice /M "Remove all of this"
if errorlevel 2 goto end

echo.
echo [1/5] Extension and video FX ...
if not defined USERPLUGINS goto end
if not defined FXDIR goto end
for %%F in (reaper_animviewer.dll reaper_fbxanimationviewer.dll reaper_fbxav_spike.dll) do (
    del /Q "%USERPLUGINS%\%%F" 2>nul
    del /Q "%USERPLUGINS%\%%F.old*" 2>nul
    del /Q "%USERPLUGINS%\%%F.new" 2>nul
)
for %%F in (rav_video_fx.clap rav_video_fx_spike.clap rav_video_fx_spike.vst3) do (
    del /Q "%FXDIR%\%%F" 2>nul
    del /Q "%FXDIR%\%%F.old*" 2>nul
    del /Q "%FXDIR%\%%F.new" 2>nul
)
if exist "%USERPLUGINS%\reaper_animviewer.dll" goto locked
if exist "%FXDIR%\rav_video_fx.clap" goto locked

echo [2/5] Launcher folder (Toolkit / ReaPack) ...
if defined SCRIPTDIR if exist "%SCRIPTDIR%\" rmdir /S /Q "%SCRIPTDIR%"

echo [3/5] Toolkit version cache ...
del /Q "%RES%\Scripts\DM_ReaperToolkit\cache\ReaAnimViewer.xml" 2>nul

echo [4/5] Launcher entry in the Actions list ...
if not exist "%RES%\reaper-kb.ini" goto kb_done
copy /Y "%RES%\reaper-kb.ini" "%RES%\reaper-kb.ini.rav-bak" >nul
powershell -NoProfile -Command "$f = '%RES%\reaper-kb.ini'; $e = [Text.Encoding]::GetEncoding(28591); $l = [IO.File]::ReadAllLines($f, $e); $k = @($l | Where-Object { $_ -notmatch 'RAV_Launcher\.lua' }); if ($k.Count -ne $l.Count) { [IO.File]::WriteAllLines($f, $k, $e); Write-Output ('      removed ' + ($l.Count - $k.Count) + ' line(s)') }"
:kb_done

echo [5/5] Test leftovers ...
if exist "%TEMP%\RAV_update_test_backup\" rmdir /S /Q "%TEMP%\RAV_update_test_backup"
if exist "%TEMP%\RAV_update_test_new\" rmdir /S /Q "%TEMP%\RAV_update_test_new"

echo.
set "OWNED=0"
ver >nul
if exist "%RES%\ReaPack\registry.db" powershell -NoProfile -Command "if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes('%RES%\ReaPack\registry.db')).Contains('reaper_animviewer.dll')) { exit 1 }"
if errorlevel 1 set "OWNED=1"
if "%OWNED%"=="1" echo [WARNING] ReaPack still lists ReaAnimViewer as installed. In REAPER:
if "%OWNED%"=="1" echo           Extensions ^> ReaPack ^> Browse packages, right-click ReaAnimViewer,
if "%OWNED%"=="1" echo           Uninstall, Apply. Otherwise the extension never updates itself.
if "%OWNED%"=="1" echo.
echo ============================================================
echo   Done. ReaAnimViewer is removed from this REAPER.
echo   In the Toolkit, the card shows as not installed.
echo ============================================================
goto end

:locked
echo.
echo [FAILED] A file is still in use. Close REAPER completely and run this again.
goto end

:no_reaper
echo [ERROR] No REAPER folder at %RES%
goto end

:reaper_running
echo [ERROR] REAPER is running. Close it, then run this file again.
goto end

:end
echo.
pause
exit /b 0
