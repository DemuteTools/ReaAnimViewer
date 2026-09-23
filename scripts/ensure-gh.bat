@echo off
rem ============================================================================
rem  Makes sure GitHub CLI (gh) is installed and logged in.
rem  Installs it with winget when missing (works on any of your PCs).
rem  On success, sets GH to the full path of gh.exe for the calling script.
rem  Usage from another script:   call "%~dp0scripts\ensure-gh.bat" || exit /b 1
rem ============================================================================

set "GH="
for /f "delims=" %%G in ('where gh 2^>nul') do if not defined GH set "GH=%%G"
if not defined GH if exist "%ProgramFiles%\GitHub CLI\gh.exe" set "GH=%ProgramFiles%\GitHub CLI\gh.exe"
if defined GH goto check_auth

echo GitHub CLI (gh) is not installed. Installing it with winget...
where winget >nul 2>nul
if errorlevel 1 (
    echo [ERROR] winget is not available on this PC.
    echo Install GitHub CLI manually from https://cli.github.com/ then run this again.
    exit /b 1
)
winget install --id GitHub.cli -e --source winget --accept-source-agreements --accept-package-agreements
if exist "%ProgramFiles%\GitHub CLI\gh.exe" set "GH=%ProgramFiles%\GitHub CLI\gh.exe"
if not defined GH (
    echo [ERROR] GitHub CLI installation failed or gh.exe was not found.
    echo Open a new terminal and run this again, or install it from https://cli.github.com/
    exit /b 1
)

:check_auth
"%GH%" auth status >nul 2>nul
if not errorlevel 1 goto ready

echo.
echo GitHub CLI is not logged in. Follow the prompts (choose GitHub.com, HTTPS,
echo and "Login with a web browser").
"%GH%" auth login
"%GH%" auth status >nul 2>nul
if errorlevel 1 (
    echo [ERROR] GitHub login failed.
    exit /b 1
)

:ready
rem Let git reuse gh's credentials for HTTPS pushes (harmless if already set).
"%GH%" auth setup-git >nul 2>nul
exit /b 0
