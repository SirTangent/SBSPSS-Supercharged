@echo off
rem Check this machine has what the build needs: port\tools\check_setup.py.
rem Usage: port\setup.cmd          (check: each prerequisite, ok or what to do)
rem        port\setup.cmd --fix    (also git lfs pull, and Visual Studio's missing
rem                                 components from the repository's .vsconfig)
setlocal
call "%~dp0findpy.cmd" || exit /b 1
%PY% -I "%~dp0tools\check_setup.py" %*
rem propagate the check's exit code to the caller
endlocal & exit /b %ERRORLEVEL%
