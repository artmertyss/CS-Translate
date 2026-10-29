@echo off
setlocal enableextensions
cd /d "%~dp0"

set "RC=0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Half-Life"

if not exist "%GAME%\hl.exe" (
    echo [ERROR] Game not found: "%GAME%"
    echo.
    echo Usage: deploy.bat "GameRootDir"
    set "RC=1"
    goto :end
)

if not exist "build\Release\cs_translate.asi" (
    echo [ERROR] build\Release\cs_translate.asi missing - build first:
    echo        cmake -S . -B build -A Win32
    echo        cmake --build build --config Release
    set "RC=1"
    goto :end
)

echo Game root : "%GAME%"
echo.

if not exist "%GAME%\winmm.dll" goto :noWinmm
set "CO="
for /f "delims=" %%i in ('powershell -NoProfile -Command "(Get-Item -LiteralPath '%GAME%\winmm.dll').VersionInfo.CompanyName" 2^>nul') do set "CO=%%i"
if "%CO%"=="ThirteenAG" (
    del /f /q "%GAME%\winmm.dll" >nul 2>&1
    if exist "%GAME%\winmm.dll" (
        echo   [WARN] winmm.dll could not be deleted - delete it manually: ThirteenAG loader
    ) else (
        echo   - winmm.dll          ThirteenAG loader removed - not needed
    )
) else (
    echo   = winmm.dll          present, untouched - not our loader
)
goto :afterWinmm
:noWinmm
echo   = winmm.dll          missing - not needed
:afterWinmm

if exist "%GAME%\cs16chattranslator.asi" (
    del /f /q "%GAME%\cs16chattranslator.asi" >nul 2>&1
    if exist "%GAME%\cs16chattranslator.asi" (
        echo   [WARN] old cs16chattranslator.asi could not be deleted - close the game and delete it
    ) else (
        echo   - cs16chattranslator.asi   old name removed
    )
)

copy /y "build\Release\cs_translate.asi" "%GAME%\cs_translate.asi" >nul
if errorlevel 1 (
    echo   [ERROR] could not copy cs_translate.asi.
    set "RC=1"
    goto :end
)
echo   + cs_translate.asi

if not exist "%GAME%\cstrike\cl_dlls\client.dll" (
    echo.
    echo   [WARN] cstrike\cl_dlls\client.dll missing - this install is for CS 1.6.
)

echo.
echo ============================================================
echo  Completed.
echo  Now start CS 1.6 normally - nothing else is needed.
echo  Open the console with the tilde key and wait for a Russian message.
echo ============================================================
echo.
echo  Details:   "%GAME%\cs_translate.log"
echo  Settings:  "%APPDATA%\cs_translate\cs_translate.ini"
echo             (kept outside the game folder on purpose)
echo.
echo  (press any key to close this window)

:end
echo.
pause
endlocal & exit /b %RC%
