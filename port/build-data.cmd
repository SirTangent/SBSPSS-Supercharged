@echo off
rem Build the SBSPSS game data (BigLump.Bin + generated headers) on Windows:
rem the CMake + Ninja data project in port\data (issue #95), through
rem port\tools\data\build_data.py.  Needs Python 3, and CMake + Ninja (MSYS2's
rem mingw32 ones, as for the PC build, or any on the PATH):
rem     pacman -S --needed mingw-w64-i686-cmake mingw-w64-i686-ninja
rem Usage: port\build-data.cmd [usa^|eur]                 (default usa; PC data is per territory - out\<T>\cd
rem                                                      serves both DEBUG and FINAL exes, issue #35)
rem        port\build-data.cmd <preset>                  (debug^|final^|usa-*^|eur-*, as build-pc.cmd; variant word ignored)
rem        port\build-data.cmd USA^|EUR DEBUG^|FINAL       (the version word is accepted and ignored)
rem        ... --vintage-tools                          (any form: the 1999 converters, not port\tools\x64 - issue #48)
setlocal
set "PY=python"
where python >nul 2>nul || set "PY=py -3"
%PY% -I "%~dp0tools\data\build_data.py" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
