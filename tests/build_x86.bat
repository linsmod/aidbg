@echo off
rem build_x86.bat - compile the x86 WoW64 aidbg test targets (TestGuid.md section 5)
rem
rem Same strategy & flags as build.cmd but with the x86 32-bit toolchain so
rem aidbg (x64) can exercise 32-bit cross-debugging.
rem (/Zi /Od /Oy- /GS- /MTd /link /DEBUG:FULL /DYNAMICBASE:NO)
rem
rem Usage:
rem   build_x86.bat        - builds all x86 targets
rem   build_x86.bat test   - builds only test_wow64.exe
rem
rem Requires the MSVC x86 toolchain. vcvars32.bat is located automatically.

setlocal enabledelayedexpansion

set "VSW=%ProgramFiles%\Microsoft Visual Studio\2022\Community"

if not exist "%VSW%\VC\Auxiliary\Build\vcvars32.bat" (
    for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul') do (
        set "VSW=%%i"
    )
)

if not exist "%VSW%\VC\Auxiliary\Build\vcvars32.bat" (
    echo build ERROR: vcvars32.bat not found. Install VS2022 or edit tests\build_x86.bat.
    exit /b 1
)

set "SRC=%~dp0src"
set "OUT=%~dp0..\"

call "%VSW%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1

set CFLAGS=/nologo /utf-8 /Zi /Od /Oy- /GS- /MTd
set LDFLAGS=/DEBUG:FULL /DYNAMICBASE:NO

set "TARGETS=test_wow64"
if not "%~1"=="" set "TARGETS=%~1"

for %%t in (%TARGETS%) do (
    echo build %%t.exe x86...
    cl %CFLAGS% /Fe"%OUT%%%t.exe" "%SRC%\%%t.c" /link %LDFLAGS% >nul
    if errorlevel 1 (
        echo build ERROR: failed to compile %%t.c
        exit /b 1
    )
)

echo build done. Outputs are next to aidbg.exe in the repo root.
exit /b 0
