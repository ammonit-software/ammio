@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0.."
set "BUILD_DIR=%ROOT_DIR%\build"

:: Set up MSVC environment (Visual Studio 2022)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Could not find Visual Studio 2022
    exit /b 1
)

:: Configure if needed (first run)
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo Configuring project...
    if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
    cd /d "%BUILD_DIR%"
    cmake -G "Visual Studio 17 2022" "%ROOT_DIR%"
    if errorlevel 1 exit /b %errorlevel%
) else (
    cd /d "%BUILD_DIR%"
)

:: Clean compiled objects if --clean flag is passed
if "%~1"=="--clean" (
    echo Cleaning build artifacts...
    cmake --build . --target clean
    shift
)

:: Build
cmake --build .
if errorlevel 1 exit /b %errorlevel%
