@echo off
rem run_tests.cmd - convenience wrapper for tests\run_tests.py.
rem
rem Usage:
rem   run_tests.cmd               build targets (if needed) + run all cases
rem   run_tests.cmd --no-build    skip the build step
rem   run_tests.cmd --case 4.2    run a single case
rem
rem The system `python`/`py` are sometimes broken on this machine, so this
rem wrapper prefers a known-good interpreter and falls back to PATH.

setlocal
set "HERE=%~dp0"

for %%p in (
  "H:\Python\Python313\python.exe"
  "%LocalAppData%\Programs\Python\Python313\python.exe"
) do (
  if exist %%p (
    set "PY=%%~p"
    goto :found
  )
)

where python >nul 2>nul && set "PY=python" && goto :found
where py >nul 2>nul && set "PY=py -3" && goto :found

echo [run_tests] ERROR: no usable Python interpreter found.
exit /b 1

:found
set "_NT_SYMBOL_PATH=%HERE%.."
"%PY%" "%HERE%run_tests.py" %*
exit /b %errorlevel%
