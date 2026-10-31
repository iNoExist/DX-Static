@echo off
rem Builds dxstatic.exe (tray app) and dxctl.exe (console tool) into .\bin
rem Needs MinGW-w64 gcc + windres on PATH (e.g. MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc).
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
set CFLAGS=-O2 -s -static -Wall -Wextra -Wno-missing-field-initializers -municode
set LIBS=-lhid -lsetupapi -lcomdlg32 -lgdi32 -lshell32 -luser32 -ladvapi32

windres src\dxstatic.rc -O coff -o bin\dxstatic.res || exit /b 1
gcc %CFLAGS% -mwindows -o bin\dxstatic.exe src\dxstatic.c bin\dxstatic.res %LIBS% || exit /b 1
gcc %CFLAGS% -mconsole -DCLI_ONLY -o bin\dxctl.exe src\dxstatic.c %LIBS% || exit /b 1
del bin\dxstatic.res
echo Built bin\dxstatic.exe and bin\dxctl.exe
