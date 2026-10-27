@echo off
rem Build the tester zip: port\package.py, under the Python findpy.cmd picks
rem (issue #98; this used MSYS2's python3).
rem Usage: port\package.cmd [--territory usa^|eur] [--out <zip>] [--build]
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% "%~dp0package.py" %*
endlocal & exit /b %ERRORLEVEL%
