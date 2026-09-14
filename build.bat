@echo off
REM SimpleRecorder build script - no third party dependencies, everything links
REM against the operating system (WASAPI + Media Foundation).
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

REM ---- locate Visual Studio -------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found - is Visual Studio installed?
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo ERROR: no Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: could not initialise the Visual Studio build environment.
    exit /b 1
)

REM ---- locate cmake ---------------------------------------------------------
set "CMAKE=cmake"
if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

REM ---- configure + build ----------------------------------------------------
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

"%CMAKE%" --build "%ROOT%\build"
if errorlevel 1 (
    echo ERROR: build failed.
    exit /b 1
)

if not exist "%ROOT%\package" mkdir "%ROOT%\package"
copy /Y "%ROOT%\build\bin\SimpleRecorder.exe" "%ROOT%\package\SimpleRecorder.exe" >nul

echo.
echo Build complete: %ROOT%\package\SimpleRecorder.exe
echo (single self-contained executable, no DLLs required)
