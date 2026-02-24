@echo off

REM Manage directories/paths
cd /d %~dp0..\
set BUILD_DIR=build
if not exist %BUILD_DIR% mkdir %BUILD_DIR%

REM Arguments defaults
set CONFIG=Debug
set CLEAN=0

REM Parse arguments
:parse_args
if "%~1"=="" goto end_parse
if /I "%~1"=="--clean" (set CLEAN=1 & shift & goto parse_args)
if /I "%~1"=="--config" (set CONFIG=%~2 & shift & shift & goto parse_args)
echo WARNING: Unknown option "%~1"
shift
goto parse_args
:end_parse

REM Manage env (skip if not found, e.g. on CI where env is already set up)
set VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat
if exist "%VSDEVCMD%" call "%VSDEVCMD%" >nul 2>&1

REM Manage cmake and build
cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64
if %CLEAN%==1 (
    cmake --build %BUILD_DIR% --config %CONFIG% --target clean
)
cmake --build %BUILD_DIR% --config %CONFIG%