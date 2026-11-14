@echo off
rem Build the SBSPSS game data (BigLump.Bin + generated headers) on Windows:
rem the CMake + Ninja data project in port\data (issue #95), through
rem port\tools\data\build_data.py.  Needs Python 3.8+, and CMake + Ninja from
rem the PATH or Visual Studio's "C++ CMake tools for Windows" (issue #98).
rem Usage: port\build-data.cmd [usa^|eur]                 (default usa; PC data is per territory - out\<T>\cd
rem                                                      serves both DEBUG and FINAL exes, issue #35)
rem        port\build-data.cmd <preset>                  (debug^|final^|usa-*^|eur-*, as build-pc.cmd; variant word ignored)
rem        port\build-data.cmd USA^|EUR DEBUG^|FINAL       (the version word is accepted and ignored)
rem        ... --vintage-tools                          (any form: the 1999 converters, not port\tools\x64 - issue #48)
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% -I "%~dp0tools\data\build_data.py" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
