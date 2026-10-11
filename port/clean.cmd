@echo off
rem Remove all build outputs and regenerable debris.
rem Everything deleted here is gitignored and rebuilt by
rem port\build-data.cmd + port\build-psx.cmd + port\build-pc.cmd.
setlocal
cd /d "%~dp0.."
set "CLEANED=0"

echo Cleaning %CD%

rem data build + PSX build outputs
call :rmdir  out
call :del    data\Scripts\defs\trans.scr
call :del    sn.ini

rem PC build: CMake/Ninja trees for every variant
call :rmdir  port\build

rem PSX build debris: generated info.mip trees and slink reports
call :rmdir  source\system\DEBUG
call :rmdir  source\system\FINAL
call :del    stats.txt
call :del    statcov.txt

if "%CLEANED%"=="0" (
    echo Nothing to clean - the tree is already clean.
) else (
    echo Cleaned %CLEANED% path^(s^).
)
endlocal & exit /b 0

:rmdir
if exist "%~1\" (
    rmdir /s /q "%~1"
    if exist "%~1\" (
        echo   FAILED to delete dir:  %~1
    ) else (
        echo   deleted dir:  %~1
        set /a CLEANED+=1
    )
)
exit /b 0

:del
if exist "%~1" (
    del /q "%~1"
    if exist "%~1" (
        echo   FAILED to delete file: %~1
    ) else (
        echo   deleted file: %~1
        set /a CLEANED+=1
    )
)
exit /b 0
