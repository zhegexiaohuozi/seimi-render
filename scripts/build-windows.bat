REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
REM seimi-render Windows build (MSVC + Ninja + Qt6), auto-inits MSVC env.
REM   scripts\build-windows.bat [Release|Debug]
REM Env vars: QT_PREFIX, QT_VERSION, QT_ARCH_DIR
setlocal enableextensions enabledelayedexpansion

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if "%QT_PREFIX%"=="" set "QT_PREFIX=C:\Qt"
if "%QT_VERSION%"=="" set "QT_VERSION=6.7.2"
if "%QT_ARCH_DIR%"=="" set "QT_ARCH_DIR=msvc2019_64"
set "QT_INSTALL_DIR=%QT_PREFIX%\%QT_VERSION%\%QT_ARCH_DIR%"
set "ROOT_DIR=%~dp0.."
set "BUILD_DIR=%ROOT_DIR%\build"
set "BIN=%BUILD_DIR%\seimi-render.exe"

echo == seimi-render build (Windows) ==
echo   root      : %ROOT_DIR%
echo   config    : %CONFIG%
echo   qt prefix : %QT_INSTALL_DIR%
echo   build dir : %BUILD_DIR%

if not exist "%QT_INSTALL_DIR%\bin\qmake.exe" (
    echo ERROR: Qt not found at %QT_INSTALL_DIR%\bin\qmake.exe >&2
    echo        run scripts\setup-windows.bat first, or set QT_PREFIX. >&2
    exit /b 1
)

REM ---- initialize MSVC environment ----
REM NOTE: %ProgramFiles(x86)% has parens that break cmd parsing; use the call trick.
call set "PF86=%%ProgramFiles(x86)%%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if "%VSINSTALL%"=="" (
    echo ERROR: MSVC toolchain not found. Install VS 2022 / Build Tools first. >&2
    exit /b 1
)
echo == initializing MSVC environment ==
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo ERROR: vcvarsall.bat x64 init failed >&2
    exit /b 1
)

REM generator: prefer Ninja, else default (VS)
set "GEN="
where ninja >nul 2>&1
if not errorlevel 1 (
    set "GEN=-G Ninja"
    echo   generator: Ninja
) else (
    echo   generator: VS (ninja not installed)
)

echo == [0/2] clean build dir ==
REM robocopy /MIR clears files even when dir cwd is locked (mspdbsrv etc.), unlike rmdir.
if exist "%BUILD_DIR%" (
    del "\\?\%BUILD_DIR%\nul" 2>nul
    set "_EMPTY_TMP=%TEMP%\_seimi_empty_rb"
    if not exist "!_EMPTY_TMP!" mkdir "!_EMPTY_TMP!"
    robocopy "!_EMPTY_TMP!" "%BUILD_DIR%" /MIR /NFL /NDL /NJH /NJS /NP >nul 2>&1
) else (
    mkdir "%BUILD_DIR%"
)

echo == [1/2] cmake configure ==
cmake %GEN% -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DCMAKE_PREFIX_PATH="%QT_INSTALL_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (
    echo ERROR: cmake configure failed >&2
    exit /b 1
)

echo == [2/2] cmake build ==
cmake --build "%BUILD_DIR%" --target seimi-render --config %CONFIG%
if errorlevel 1 (
    echo ERROR: cmake build failed >&2
    exit /b 1
)

if not exist "%BIN%" (
    echo ERROR: binary not produced at %BIN% >&2
    exit /b 1
)

echo.
echo == DONE ==
echo   binary: %BIN%
echo.
echo   run:
echo     "%BIN%" --http-port 8088 --ws-port 8089
endlocal
