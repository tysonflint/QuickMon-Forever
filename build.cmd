@echo off
rem Builds QuickMonForever.scr (64-bit screen saver) and QuickMonForever-32.exe (32-bit counter helper).
rem Needs Visual Studio 2022 (or newer) with the "Desktop development with C++" workload.
rem Hardening: /GS stack checks, /guard:cf Control Flow Guard, /sdl extra security checks;
rem /DYNAMICBASE (ASLR), /NXCOMPAT (DEP), /CETCOMPAT (shadow stack), and
rem /DEPENDENTLOADFLAG:0x800 (the system DLLs it links to load only from System32).
setlocal
cd /d "%~dp0"

set "VSROOT="
for %%V in ("%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\18\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise") do (
    if not defined VSROOT if exist "%%~V\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%~V"
)
if not defined VSROOT (
    echo Visual Studio with C++ tools was not found.
    exit /b 1
)

if not exist build mkdir build
set "CLFLAGS=/nologo /std:c++17 /O2 /W4 /EHsc /MT /DUNICODE /D_UNICODE /GS /guard:cf /sdl"
set "LIBS=user32.lib gdi32.lib advapi32.lib pdh.lib comctl32.lib"

echo === 64-bit screen saver ===
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\x64 mkdir build\x64
rc /nologo /fo build\x64\QuickMonForever.res src\QuickMonForever.rc || exit /b 1
cl %CLFLAGS% /Fobuild\x64\ src\QuickMonForever.cpp build\x64\QuickMonForever.res /link /OUT:build\QuickMonForever.scr /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /guard:cf /CETCOMPAT /DEPENDENTLOADFLAG:0x800 %LIBS% || exit /b 1

endlocal & setlocal
cd /d "%~dp0"
set "VSROOT="
for %%V in ("%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\18\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise") do (
    if not defined VSROOT if exist "%%~V\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%%~V"
)
set "CLFLAGS=/nologo /std:c++17 /O2 /W4 /EHsc /MT /DUNICODE /D_UNICODE /GS /guard:cf /sdl"
set "LIBS=user32.lib gdi32.lib advapi32.lib pdh.lib comctl32.lib"

echo === 32-bit counter helper ===
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
if not exist build\x86 mkdir build\x86
rc /nologo /fo build\x86\QuickMonForever.res src\QuickMonForever.rc || exit /b 1
cl %CLFLAGS% /Fobuild\x86\ src\QuickMonForever.cpp build\x86\QuickMonForever.res /link /OUT:build\QuickMonForever-32.exe /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /guard:cf /CETCOMPAT /DEPENDENTLOADFLAG:0x800 %LIBS% || exit /b 1

echo.
echo Built: build\QuickMonForever.scr and build\QuickMonForever-32.exe
endlocal
