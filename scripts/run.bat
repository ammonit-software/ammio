@echo off
REM Usage: scripts\run.bat [--release] <config.json> [ammio options]

cd /d %~dp0..\
set "CONFIG=Debug"
if /I "%~1"=="--release" (set "CONFIG=Release" & shift)

"build\%CONFIG%\ammio.exe" %1 %2 %3 %4 %5 %6 %7 %8 %9
