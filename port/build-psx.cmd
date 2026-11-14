@echo off
rem Build the original PlayStation executable (Spongey.cpe) on Windows:
rem port\tools\psx\build_psx.py, which builds port\psx's CMake + Ninja project
rem over the 1999 PSY-Q toolchain in tools\psyq (issue #98; until then this
rem started MSYS2's bash for build-psx.sh and makefile.gaz).  Needs Python
rem 3.8+, CMake + Ninja from the PATH or Visual Studio's "C++ CMake tools
rem for Windows", and a prior data build (the generated headers in
rem out\<TERRITORY>\include are compile inputs).
rem Usage: port\build-psx.cmd [USA^|EUR] [DEBUG^|FINAL] [--identity-noline]    (defaults: USA DEBUG)
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% -I "%~dp0tools\psx\build_psx.py" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
