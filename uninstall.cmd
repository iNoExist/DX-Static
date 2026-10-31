@echo off
rem Removes dxstatic: stops the tray app, removes the startup entry and saved colour,
rem deletes the installed files.  The strip keeps its current colour until you start
rem DX Light again, press the strip's button, or it loses power.
setlocal
set DEST=%LOCALAPPDATA%\Programs\DXStatic
taskkill /im dxstatic.exe >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v DXStatic /f >nul 2>&1
reg delete "HKCU\Software\DXStatic" /f >nul 2>&1
if exist "%DEST%" rmdir /s /q "%DEST%"
echo dxstatic removed.
