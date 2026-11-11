@echo off
rem Build the original PlayStation executable (Spongey.cpe) on Windows:
rem port\tools\psx\build_psx.py, which builds port\psx's CMake + Ninja project
rem over the 1999 PSY-Q toolchain in tools\psyq (issue #98; until then this
rem started MSYS2's bash for build-psx.sh and makefile.gaz).  Needs Python
rem 3.8+, CMake + Ninja from the PATH or Visual Studio's "C++ CMake tools
rem for Windows".  The generated headers in out\<TERRITORY>\include are
rem compile inputs, so it builds the territory's data first, as
rem port\build-data.cmd does; --no-data leaves out\<TERRITORY> as it is.
rem Usage: port\build-psx.cmd [USA^|EUR] [DEBUG^|FINAL] [--identity-noline] [--no-data]    (defaults: USA DEBUG)
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% -I "%~dp0tools\psx\build_psx.py" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
