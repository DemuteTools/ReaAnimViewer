@echo off
setlocal EnableExtensions EnableDelayedExpansion
title ReaAnimViewer - Dev files sync

rem ============================================================================
rem  Syncs the PRIVATE dev files between your PCs through a private GitHub repo
rem  (<owner>/ReaAnimViewer-dev). These files are ignored by the public repo:
rem    .claude\  _bmad\  _bmad-output\  CLAUDE.md  sample\ (test models)
rem    docs\PHASE*_VALIDATOR_GATE.md  docs\SPIKE0_FINDINGS.md
rem  (.claude\settings.local.json stays local to each PC)
rem
rem  Usage:  dev-sync.bat [setup|push|pull|status]
rem    setup   First time on a PC. Creates the private repo if it does not exist,
rem            otherwise downloads the dev files from it.
rem    push    Send your local dev file changes to the private repo.
rem    pull    Get the latest dev files from the private repo.
rem    status  Show what changed locally.
rem  Double-click = menu.
rem
rem  Tip: push before leaving a PC, pull when you arrive on another one.
rem  How it works: a second git repository stored in .devgit\ shares this folder
rem  as work tree. The public repo never sees it.
rem ============================================================================

cd /d "%~dp0"

set "DEVGIT=.devgit"
set "DEVREPO_NAME=ReaAnimViewer-dev"
set "G=git --git-dir=%DEVGIT% --work-tree=."
set "LOCAL_ONLY=.claude/settings.local.json .claude/worktrees"

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git was not found in PATH.
    goto failed
)

rem Turn on the automatic sync on this PC: from now on `git pull` also pulls the
rem dev files and `git push` also pushes them (.githooks\dev-sync.sh).
git config core.hooksPath .githooks

set "ACTION=%~1"
if not "%ACTION%"=="" goto dispatch

echo ============================================================
echo   ReaAnimViewer - private dev files sync
echo ============================================================
echo   1. push    send my local changes
echo   2. pull    get the latest version
echo   3. status  show local changes
echo   4. setup   first time on this PC
echo   Q. quit
echo.
choice /C 1234Q /N /M "Choice: "
if errorlevel 5 exit /b 0
if errorlevel 4 set "ACTION=setup"  & goto dispatch
if errorlevel 3 set "ACTION=status" & goto dispatch
if errorlevel 2 set "ACTION=pull"   & goto dispatch
set "ACTION=push"

:dispatch
if /I "%ACTION%"=="setup"  goto setup
if not exist "%DEVGIT%\" (
    echo This PC is not set up yet. Running setup first.
    goto setup
)
if /I "%ACTION%"=="push"   goto push
if /I "%ACTION%"=="pull"   goto pull
if /I "%ACTION%"=="status" goto status
echo [ERROR] Unknown action: %ACTION%
echo Usage: dev-sync.bat [setup^|push^|pull^|status]
goto failed

rem ----------------------------------------------------------------------------
:setup
if exist "%DEVGIT%\" (
    echo Already set up on this PC ^(%DEVGIT%\ exists^).
    goto done
)
call "%~dp0tools\ensure-gh.bat" || goto failed

rem The private repo lives on YOUR GitHub account (the one gh is logged in with),
rem not in the DemuteTools organization: every user who signs up for the tools
rem becomes a member there and could read it.
set "OWNER="
for /f "delims=" %%O in ('call "%GH%" api user -q .login 2^>nul') do set "OWNER=%%O"
if not defined OWNER (
    echo [ERROR] Could not read your GitHub user name from GitHub CLI.
    goto failed
)
set "DEVREPO=%OWNER%/%DEVREPO_NAME%"
set "DEVURL=https://github.com/%DEVREPO%.git"

"%GH%" repo view "%DEVREPO%" >nul 2>nul
if errorlevel 1 goto setup_create

rem --- The private repo exists: download the dev files on this PC -----------
echo Private repo found: %DEVREPO%
echo The dev files of this folder will be REPLACED by the version from GitHub.
choice /M "Continue"
if errorlevel 2 goto failed
git clone --bare "%DEVURL%" "%DEVGIT%" || goto failed
call :configure_devgit
%G% fetch origin || goto failed
%G% reset --hard origin/main || goto failed
%G% branch --set-upstream-to=origin/main main >nul
echo.
call :mark_synced
echo OK: dev files downloaded from %DEVREPO%.
goto done

rem --- First PC: create the private repo and upload the dev files ----------
:setup_create
echo Creating the private repo %DEVREPO% ...
"%GH%" repo create "%DEVREPO%" --private --description "Private dev files for ReaAnimViewer (BMAD, Claude, validation docs)" || goto failed
git init --bare "%DEVGIT%" >nul || goto failed
%G% remote add origin "%DEVURL%" || goto failed
call :configure_devgit
%G% symbolic-ref HEAD refs/heads/main
call :stage_dev_files || goto failed
%G% commit -q -m "Initial dev files" || goto failed
%G% push -u origin main || goto failed
echo.
call :mark_synced
echo OK: private repo created and dev files uploaded: https://github.com/%DEVREPO%
goto done

rem ----------------------------------------------------------------------------
:push
call :stage_dev_files || goto failed
%G% diff --cached --quiet
if errorlevel 1 (
    %G% commit -q -m "Sync dev files from %COMPUTERNAME%" || goto failed
) else (
    echo No local changes to commit.
)
rem Merge what the other PCs pushed first, so this push cannot be rejected.
call :fetch_remote || goto failed
%G% merge --no-edit origin/main
if errorlevel 1 (
    echo.
    echo [FAILED] Both PCs changed the same dev file. Fix the conflicts, then run:
    echo   dev-sync.bat push
    goto failed
)
%G% push origin main || goto failed
echo.
call :mark_synced
echo OK: dev files pushed.
goto done

:pull
call :fetch_remote || goto failed
%G% merge --no-edit origin/main
if errorlevel 1 (
    echo.
    echo [FAILED] Both PCs changed the same dev file. Fix the conflicts, then run:
    echo   dev-sync.bat push
    goto failed
)
echo.
call :mark_synced
echo OK: dev files up to date.
goto done

:status
%G% status --short
goto done

rem ----------------------------------------------------------------------------
rem Records the last successful sync: a dev file newer than this marker has not
rem been sent yet (.claude\hooks\dev-sync-reminder.sh reminds the user).
:mark_synced
type nul > "%DEVGIT%\last-sync"
exit /b 0

rem Downloads the private repo. Kept apart from the merge so that an access or
rem network error is not reported as a conflict.
:fetch_remote
%G% fetch origin
if errorlevel 1 (
    echo.
    echo [FAILED] Could not reach the private repo. Check that git on Windows
    echo can sign in to GitHub ^(remote: https://github.com/^<you^>/%DEVREPO_NAME%.git^).
    exit /b 1
)
exit /b 0

rem Settings of the private repo on this PC.
:configure_devgit
%G% config core.bare false
%G% config status.showUntrackedFiles no
%G% config remote.origin.fetch "+refs/heads/*:refs/remotes/origin/*"
exit /b 0

rem Stages every dev file: changes and deletions of tracked files, plus new
rem files in the dev folders. -f is needed because the public .gitignore
rem ignores these paths on purpose.
:stage_dev_files
%G% add -u || exit /b 1
set "PATHS="
for %%P in (.claude _bmad _bmad-output CLAUDE.md sample) do if exist "%%P" set "PATHS=!PATHS! %%P"
for %%P in (docs\PHASE*_VALIDATOR_GATE.md docs\SPIKE0_FINDINGS.md) do if exist "%%P" set "PATHS=!PATHS! %%P"
if defined PATHS %G% add -f -- !PATHS! || exit /b 1
%G% rm -r -q --cached --ignore-unmatch -- %LOCAL_ONLY% >nul
exit /b 0

rem ----------------------------------------------------------------------------
:done
echo.
if "%~1"=="" pause
exit /b 0

:failed
echo.
if "%~1"=="" pause
exit /b 1
