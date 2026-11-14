@echo off
rem Sets PY to a Python 3.8+ command for the port's .cmd scripts (issue #98):
rem the python.org launcher (py -3) when it works, else python.  Each is tried
rem by running it, because the Microsoft Store's python.exe alias sits on the
rem PATH but only opens the Store.  Used as:  call "%~dp0findpy.cmd" || exit /b 1
set "PY="
py -3 -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && (set "PY=py -3" & exit /b 0)
python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && (set "PY=python" & exit /b 0)
echo No Python 3.8 or newer: install it from https://www.python.org and put it on the PATH. >&2
exit /b 1
