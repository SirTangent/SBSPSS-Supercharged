@echo off
rem Build the original PlayStation executable (Spongey.cpe) on Windows.
rem Requires MSYS2 (C:\msys64) with make installed, and a prior data build
rem (the generated headers in out\<TERRITORY>\include are compile inputs).
rem Usage: port\build-psx.cmd [USA^|EUR] [DEBUG^|FINAL]    (defaults: USA DEBUG)
rem The entry point from cmd, PowerShell or Git Bash: build-psx.sh itself only
rem runs in MSYS2's bash (issue #51), which this starts as an MSYS login shell
rem whatever MSYSTEM the caller has (Git Bash sets MINGW64).
setlocal
if not defined MSYS2_WIN set "MSYS2_WIN=C:\msys64"
set "MSYSTEM=MSYS"
set "SCRIPT=%~dp0build-psx.sh"
set "SCRIPT=%SCRIPT:\=/%"
"%MSYS2_WIN%\usr\bin\bash.exe" -l "%SCRIPT%" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
