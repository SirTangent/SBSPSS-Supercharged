@echo off
rem Build the tester zip (port\package.py) with MSYS2's own python3, so it
rem works on a machine whose `python` is the Microsoft Store alias stub.
rem Usage: port\package.cmd [--territory usa^|eur] [--out <zip>] [--build]
setlocal
if not defined MSYS2_WIN set "MSYS2_WIN=C:\msys64"
set "PY=%MSYS2_WIN%\mingw32\bin\python3.exe"
if not exist "%PY%" set "PY=%MSYS2_WIN%\mingw64\bin\python3.exe"
if not exist "%PY%" (
    echo package.cmd: no python3 under %MSYS2_WIN% - install it: pacman -S mingw-w64-i686-python
    exit /b 1
)
"%PY%" "%~dp0package.py" %*
endlocal & exit /b %ERRORLEVEL%
