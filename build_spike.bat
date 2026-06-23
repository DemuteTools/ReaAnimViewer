@echo off
setlocal
title Spike 0 - Build et Install

echo ============================================================
echo   FBXAV Spike 0 - compilation + installation dans Reaper
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
if errorlevel 1 (
  echo.
  echo [ERREUR] CMake est introuvable dans le PATH.
  echo Reinstalle CMake (Etape 1b du guide) en cochant
  echo "Add CMake to the system PATH for all users".
  echo.
  pause
  exit /b 1
)

echo.
echo [1/3] Configuration du projet (telecharge assimp/GLM la 1re fois)...
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
  echo.
  echo [ECHEC] La configuration CMake a echoue.
  echo Copie-moi le texte d'erreur ci-dessus.
  echo.
  pause
  exit /b 1
)

echo.
echo [2/3] Compilation (peut prendre plusieurs minutes la 1re fois)...
cmake --build build --config Release
if errorlevel 1 (
  echo.
  echo [ECHEC] La compilation a echoue.
  echo Copie-moi la PREMIERE ligne contenant "error" ci-dessus.
  echo.
  pause
  exit /b 1
)

echo.
echo [3/3] Installation de la DLL dans Reaper...
copy /Y "build\Release\reaper_fbxav_spike.dll" "%APPDATA%\REAPER\UserPlugins\"
if errorlevel 1 (
  echo.
  echo [ECHEC] Copie de la DLL impossible.
  echo Verifie que REAPER est bien ferme, puis relance ce script.
  echo.
  pause
  exit /b 1
)

echo.
echo ============================================================
echo   OK ! DLL compilee et installee.
echo.
echo   -^> Ouvre REAPER, puis lance l'action :
echo      "FBXAV: Open Spike Viewer"
echo ============================================================
echo.
pause
