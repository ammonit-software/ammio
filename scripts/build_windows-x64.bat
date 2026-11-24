@echo off
REM Usage: scripts\build_windows-x64.bat [--clean] [--config Debug|Release]

cd /d %~dp0..\
set "BUILD_DIR=build"
set "CONFIG=Debug"
set "CLEAN=0"

:parse_args
if "%~1"=="" goto end_parse
if /I "%~1"=="--clean"  (set "CLEAN=1" & shift & goto parse_args)
if /I "%~1"=="--config" (set "CONFIG=%~2" & shift & shift & goto parse_args)
echo WARNING: unknown option "%~1"
shift
goto parse_args
:end_parse

set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not defined VSCMD_VER (
    if exist "%VSDEVCMD%" call "%VSDEVCMD%" >nul 2>&1
)

cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 || exit /b 1
if %CLEAN%==1 cmake --build %BUILD_DIR% --config %CONFIG% --target clean
cmake --build %BUILD_DIR% --config %CONFIG% || exit /b 1
