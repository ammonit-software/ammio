@echo off

REM Manage directories/paths
cd /d %~dp0..\
set BUILD_DIR=build

REM Arguments defaults
set CONFIG=Debug

REM Parse arguments
:parse_args
if "%~1"=="" goto end_parse
if /I "%~1"=="--config" (set CONFIG=%~2 & shift & shift & goto parse_args)
echo WARNING: Unknown option "%~1"
shift
goto parse_args
:end_parse

REM Manage run
"%BUILD_DIR%\%CONFIG%\ammio.exe" --config config\config.json --interface config\interface.json