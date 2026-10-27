@echo off
rem Build the PC port: port\tools\build_pc.py (issue #98 replaced build-pc.sh
rem with it; the command line is the same).  Needs Python 3.8+, and CMake +
rem Ninja from the PATH or Visual Studio's "C++ CMake tools for Windows".
rem Usage: port\build-pc.cmd [debug^|final^|usa-debug^|usa-final^|eur-debug^|eur-final^|clangcl-debug^|clangcl-final^|clangcl-eur-debug^|clangcl-eur-final^|clangcl-x64-debug^|clangcl-x64-final^|clangcl-x64-eur-debug^|clangcl-x64-eur-final^|usa^|eur^|clangcl [usa^|eur]^|clangcl64 [usa^|eur]^|all^|test [usa^|eur^|clangcl [usa^|eur]^|clangcl64 [usa^|eur]]^|soak [usa^|eur^|clangcl [usa^|eur]^|clangcl64 [usa^|eur]]^|parity64 [usa^|eur] [debug^|final]]
rem        (build the territory's data first: port\build-data.cmd usa ^| eur - one build serves DEBUG and FINAL)
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% -I "%~dp0tools\build_pc.py" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
