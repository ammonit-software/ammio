@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0.."
set "BUILD_DIR=%ROOT_DIR%\build"

:: Run
cd /d "%ROOT_DIR%"
"%ROOT_DIR%\build\Debug\ammio.exe" config\config.json config\interface.json
