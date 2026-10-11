@echo off
rem Build the PC (Win32) port via MSYS2 bash.
rem Usage: port\build-pc.cmd [debug^|final^|usa-debug^|usa-final^|eur-debug^|eur-final^|clangcl-debug^|clangcl-final^|clangcl-eur-debug^|clangcl-eur-final^|clangcl-x64-debug^|clangcl-x64-final^|clangcl-x64-eur-debug^|clangcl-x64-eur-final^|usa^|eur^|clangcl [usa^|eur]^|clangcl64 [usa^|eur]^|all^|test [usa^|eur^|clangcl [usa^|eur]^|clangcl64 [usa^|eur]]^|soak [usa^|eur^|clangcl [usa^|eur]^|clangcl64 [usa^|eur]]^|parity64 [usa^|eur] [debug^|final]]
rem        (build the territory's data first: port\build-data.cmd usa ^| eur - one build serves DEBUG and FINAL)
setlocal
if not defined MSYS2_WIN set "MSYS2_WIN=C:\msys64"
set "SCRIPT=%~dp0build-pc.sh"
set "SCRIPT=%SCRIPT:\=/%"
"%MSYS2_WIN%\usr\bin\bash.exe" -l "%SCRIPT%" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
