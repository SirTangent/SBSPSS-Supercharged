@echo off
rem Build the PC (Win32) port via MSYS2 bash.
rem Usage: port\build-pc.cmd [debug^|final^|usa-debug^|usa-final^|eur-debug^|eur-final^|clangcl-debug^|clangcl-final^|clangcl-x64-debug^|clangcl-x64-final^|usa^|eur^|clangcl^|clangcl64^|all^|test [usa^|eur^|clangcl^|clangcl64]^|soak [usa^|eur^|clangcl^|clangcl64]^|parity64 [debug^|final]]
rem        (build the territory's data first: port\build-data.cmd usa ^| eur - one build serves DEBUG and FINAL)
setlocal
if not defined MSYS2_WIN set "MSYS2_WIN=C:\msys64"
set "SCRIPT=%~dp0build-pc.sh"
set "SCRIPT=%SCRIPT:\=/%"
"%MSYS2_WIN%\usr\bin\bash.exe" -l "%SCRIPT%" %*
rem propagate the build's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
