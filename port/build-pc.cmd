@echo off
rem Build the PC port: port\tools\build_pc.py (issue #98 replaced build-pc.sh
rem with it; the command line is the same).  Needs Python 3.8+, and CMake +
rem Ninja from the PATH or Visual Studio's "C++ CMake tools for Windows".
rem Usage: port\build-pc.cmd [debug^|final^|usa-debug^|usa-final^|eur-debug^|eur-final^|x86-debug^|x86-final^|x86-eur-debug^|x86-eur-final^|usa^|eur^|x86 [usa^|eur]^|all^|test [usa^|eur^|x86 [usa^|eur]^|^<preset^>]^|soak [usa^|eur^|x86 [usa^|eur]^|^<preset^>]^|parity64 [usa^|eur] [debug^|final]]
rem        (every preset is clang-cl: debug/final/eur-* are x64, the exes that ship; x86-* are 32-bit - issue #39)
rem        (build the territory's data first: port\build-data.cmd usa ^| eur - one build serves DEBUG and FINAL)
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% -I "%~dp0tools\build_pc.py" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
