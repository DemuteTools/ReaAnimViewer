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
echo [1/4] Configuration du projet...
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto err_cfg

echo.
echo [2/4] Compilation (peut prendre une minute la 1re fois)...
cmake --build build --config Release
if errorlevel 1 goto err_build

echo.
echo [3/4] Purge des anciennes DLL pre-renommage...
rem Le projet s'appelait "fbxanimationviewer" avant le renommage (story 1.1).
rem Une vieille reaper_fbxanimationviewer.dll (ou la DLL du spike) qui traine
rem dans UserPlugins est chargee EN PLUS de la neuve par Reaper -> tu revois
rem les logs [FBXAV] au demarrage. On les supprime ici (sans erreur si absentes).
del /Q "%APPDATA%\REAPER\UserPlugins\reaper_fbxanimationviewer.dll" 2>nul
del /Q "%APPDATA%\REAPER\UserPlugins\reaper_fbxav_spike.dll" 2>nul

echo.
echo [4/4] Installation de la DLL dans Reaper...
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
