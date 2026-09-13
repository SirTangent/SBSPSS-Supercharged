@echo off
rem Build the SBSPSS game data (BigLump.Bin + generated headers) on Windows.
rem Requires MSYS2 (C:\msys64) with make and perl installed:
rem     pacman -S --needed make perl
rem Usage: port\build-data.cmd [TERRITORY] [VERSION]     (defaults: USA DEBUG)
rem        port\build-data.cmd <preset>                  (debug^|final^|usa-debug^|usa-final^|eur-debug^|eur-final, as build-pc.cmd)
setlocal
if not defined MSYS2_WIN set "MSYS2_WIN=C:\msys64"
set "SCRIPT=%~dp0build-data.sh"
set "SCRIPT=%SCRIPT:\=/%"
"%MSYS2_WIN%\usr\bin\bash.exe" -l "%SCRIPT%" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
