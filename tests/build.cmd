@echo off
rem build.cmd - compile the aidbg test targets (TestGuid.md section 5).
rem
rem Strategy B (internal full-symbol build) is used for every target so that
rem `info locals`, `info args`, `list` and `break <symbol>` are reliable:
rem   /Zi /Od /Oy- /GS- /MTd ... /link /DEBUG:FULL /DYNAMICBASE:NO
rem
rem Usage:  build.cmd          (builds all targets)
rem          build.cmd test    (builds only test_basic.exe)
rem
rem Requires the MSVC x64 toolchain (vcvars64.bat is located automatically).

setlocal
set "VSW=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not exist "%VSW%\VC\Auxiliary\Build\vcvars64.bat" (
  for /f "delims=" %%i in ('dir /b /s "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" 2^>nul') do call :find_vswhere "%%i"
)
if not exist "%VSW%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [build] ERROR: vcvars64.bat not found. Install VS2022 or edit tests\build.cmd.
  exit /b 1
)

set "SRC=%~dp0src"
set "OUT=%~dp0..\"

call "%VSW%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set "CFLAGS=/nologo /utf-8 /Zi /Od /Oy- /GS- /MTd"
set "LDFLAGS=/DEBUG:FULL /DYNAMICBASE:NO"

rem Rebuild "clean" first so stale PDBs never mask symbol bugs.
rem (Per-target build below only recompiles when the source changed.)

set "TARGETS=test_basic test_memory test_exception test_threads test_symbols test_attach test_checksum test_debugbreak test_source_step test_vars test_string"
if not "%~1"=="" set "TARGETS=%~1"

for %%t in (%TARGETS%) do (
  echo [build] %%t.exe ...
  cl %CFLAGS% "/Fe:%OUT%%%t.exe" "%SRC%\%%t.c" /link %LDFLAGS% >nul
  if errorlevel 1 (
    echo [build] ERROR: failed to compile %%t.c
    exit /b 1
  )
)

echo [build] done. Outputs are next to aidbg.exe in the repo root.
exit /b 0
