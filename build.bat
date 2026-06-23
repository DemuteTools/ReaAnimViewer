@echo off
setlocal
title ReaAnimViewer - Build et Install

echo ============================================================
echo   ReaAnimViewer Phase 0 - compilation + installation Reaper
echo ============================================================
echo.
echo   IMPORTANT : ferme REAPER avant de continuer (sinon la DLL
echo   est verrouillee et la copie echouera).
echo.
pause

rem Se placer dans le dossier du projet (la ou est ce .bat), quelle que
rem soit la facon dont il a ete lance.
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 goto err_cmake

echo.
echo [1/3] Configuration du projet...
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto err_cfg

echo.
echo [2/3] Compilation (peut prendre une minute la 1re fois)...
cmake --build build --config Release
if errorlevel 1 goto err_build

echo.
echo [3/3] Installation de la DLL dans Reaper...
copy /Y "build\Release\reaper_animviewer.dll" "%APPDATA%\REAPER\UserPlugins\"
if errorlevel 1 goto err_copy

echo.
echo ============================================================
echo   OK ! DLL compilee et installee.
echo   -^> Ouvre REAPER puis lance l'action "RAV: Open Viewer"
echo ============================================================
goto done

:err_cmake
echo.
echo [ERREUR] CMake est introuvable dans le PATH.
echo Reinstalle CMake en cochant "Add CMake to the system PATH
echo for all users", puis relance ce script.
goto done

:err_cfg
echo.
echo [ECHEC] La configuration CMake a echoue.
echo Copie-moi le texte d'erreur affiche ci-dessus.
goto done

:err_build
echo.
echo [ECHEC] La compilation a echoue.
echo Copie-moi la PREMIERE ligne contenant "error" ci-dessus.
goto done

:err_copy
echo.
echo [ECHEC] Copie de la DLL impossible.
echo Verifie que REAPER est bien ferme, puis relance ce script.
goto done

:done
echo.
pause
