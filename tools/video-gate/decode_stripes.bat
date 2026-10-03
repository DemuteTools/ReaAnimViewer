@echo off
setlocal EnableExtensions
title RAV - Video FX sync check

rem ============================================================================
rem  Video FX sync check (Epic 11 gate). Double-click it and pick the rendered
rem  video, or drag the video file onto this .bat.
rem  The video must be rendered with the RAV video FX on the track and the action
rem  "RAV: Video FX test pattern" ON. Every frame's stripe code must equal its
rem  project frame index: type the first one (region start x frame rate) when
rem  asked. The background must be orange. Writes <video>.stripes.csv next to it.
rem  Installs ffmpeg with winget on first use. Logic: decode_stripes.ps1
rem ============================================================================

cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0decode_stripes.ps1" "%~1"

echo.
pause
