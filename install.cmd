@echo off
rem Installs dxstatic for the current user (no admin needed), enables start with
rem Windows and starts the tray app.  Run build.cmd first.
setlocal
set DEST=%LOCALAPPDATA%\Programs\DXStatic
cd /d "%~dp0"
if not exist bin\dxstatic.exe (echo bin\dxstatic.exe missing - run build.cmd first & exit /b 1)
taskkill /im dxstatic.exe >nul 2>&1
if not exist "%DEST%" mkdir "%DEST%"
copy /y bin\dxstatic.exe "%DEST%\" >nul || exit /b 1
copy /y bin\dxctl.exe "%DEST%\" >nul || exit /b 1
"%DEST%\dxctl.exe" --startup on || exit /b 1
start "" "%DEST%\dxstatic.exe"
echo Installed to %DEST%
