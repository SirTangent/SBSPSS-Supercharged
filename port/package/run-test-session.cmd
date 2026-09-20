@echo off
rem One recorded test session of the PC port (M8 shell).
rem
rem   run-test-session.cmd            play sbsp-debug.exe (asserts are live)
rem   run-test-session.cmd final      play sbsp.exe, the FINAL build
rem   run-test-session.cmd x64        play sbsp64-debug.exe, the 64-bit build
rem   run-test-session.cmd x64 final  play sbsp64.exe
rem (the 64-bit exes are only in a zip built with them; the words go in
rem that order)
rem
rem Everything the session produces lands in sessions\<date-time>\ :
rem   card-before.mcd / card-after.mcd   the memory card around the session
rem   session.pad                        the input recording (--record-pad,
rem                                      replayable with --pad-file)
rem   stdout.txt / stderr.txt            the game's and the shim's logs, kept
rem                                      apart (they must not be merged: the
rem                                      shim's tagged lines are in stderr)
rem When you are done, zip that folder and send it with your notes.
setlocal
cd /d "%~dp0"

set "BASE=sbsp"
set "VARIANT=%~1"
if /i "%~1"=="x64" (
    set "BASE=sbsp64"
    set "VARIANT=%~2"
)
set "EXE=%BASE%-debug.exe"
if /i "%VARIANT%"=="final" set "EXE=%BASE%.exe"
if not exist "%EXE%" (
    echo %EXE% is not here - run this script from the unpacked zip folder.
    exit /b 2
)

rem (%date% is locale-shaped; ask PowerShell for a fixed stamp instead)
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%i"
if not defined STAMP set "STAMP=%RANDOM%"
set "SESSION=sessions\%STAMP%"
mkdir "%SESSION%" 2>nul
if not exist saves mkdir saves
if exist saves\card0.mcd copy /y saves\card0.mcd "%SESSION%\card-before.mcd" >nul

echo Session folder: %SESSION%
echo Running %EXE% - close the game window when you are done.
echo.
rem (absolute path: cmd does not always search the current directory)
"%~dp0%EXE%" --record-pad "%SESSION%\session.pad" --assert-continue --mem-log 1>"%SESSION%\stdout.txt" 2>"%SESSION%\stderr.txt"
set "CODE=%ERRORLEVEL%"

if exist saves\card0.mcd copy /y saves\card0.mcd "%SESSION%\card-after.mcd" >nul
echo.
echo Exit code %CODE%   (0 clean, 10 assert, 11 crash, 12 watchdog, 13 replay)
findstr /b /c:"[summary]" "%SESSION%\stderr.txt"
findstr /b /c:"[assert]" /c:"[crash]" /c:"[watchdog]" "%SESSION%\stderr.txt" >nul && (
    echo Problems were logged - see %SESSION%\stderr.txt
)
echo.
echo Please zip the folder %SESSION% and send it with a note of what you did.
endlocal & exit /b %CODE%
