REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
REM seimi-render Windows 构建环境准备 (Win10/11 x64)
REM   1) 检测 MSVC + Windows SDK（VS Build Tools 或完整 VS 均可）
REM   2) winget 装 CMake + Ninja（缺失时）
REM   3) aqtinstall 装 Qt 6.7.2（含 webengine 等）到 C:\Qt
REM 前置: VS 2022/Build Tools 已装（含 MSVC v143 + Windows SDK）。
REM 用法: scripts\setup-windows.bat  环境变量: QT_VERSION, QT_PREFIX。幂等。
setlocal enableextensions enabledelayedexpansion

if "%QT_VERSION%"=="" set "QT_VERSION=6.7.2"
if "%QT_PREFIX%"=="" set "QT_PREFIX=C:\Qt"
REM aqt 参数用 win64_msvc2019_64，但实际装到 C:\Qt\6.7.2\msvc2019_64（无 win64_ 前缀）。
if "%QT_ARCH%"=="" set "QT_ARCH=win64_msvc2019_64"
if "%QT_ARCH_DIR%"=="" set "QT_ARCH_DIR=msvc2019_64"
set "QT_INSTALL_DIR=%QT_PREFIX%\%QT_VERSION%\%QT_ARCH_DIR%"

echo == seimi-render Windows setup ==
echo   qt version : %QT_VERSION%
echo   qt arch    : %QT_ARCH%
echo   qt prefix  : %QT_PREFIX%

REM ---- 1. MSVC toolchain ----
echo == [1/3] locate MSVC toolchain ==
REM NOTE: %ProgramFiles(x86)% 的括号会破坏 cmd 解析，用 call trick。
call set "PF86=%%ProgramFiles(x86)%%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo   ERROR: vswhere.exe not found. Install VS 2022 or Build Tools first. >&2
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if "%VSINSTALL%"=="" (
    echo   ERROR: no VS install with MSVC C++ tools found. >&2
    echo          Enable "Desktop development with C++" in VS Installer. >&2
    exit /b 1
)
echo   VS install: %VSINSTALL%
REM 用 flag 变量检测 vcvarsall，避免在 if 块里展开含括号的路径。
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS%" goto :vc_ok
echo   ERROR: vcvarsall.bat not found under %VSINSTALL% >&2
exit /b 1
:vc_ok
echo   [OK] MSVC toolchain present

REM ---- 2. CMake + Ninja via winget ----
echo == [2/3] CMake and Ninja ==
where cmake >nul 2>&1
if errorlevel 1 (
    echo   installing CMake via winget...
    winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements -h --disable-interactivity >nul 2>&1
)
where cmake >nul 2>&1 && (echo   [OK] cmake) || (echo   [WARN] cmake still not on PATH)

where ninja >nul 2>&1
if errorlevel 1 (
    echo   installing Ninja via winget...
    winget install --id Ninja-build.Ninja -e --accept-source-agreements --accept-package-agreements -h --disable-interactivity >nul 2>&1
)
where ninja >nul 2>&1 && (echo   [OK] ninja) || (echo   [WARN] ninja not on PATH; cmake will fall back to another generator)

REM ---- 3. aqtinstall + Qt ----
echo == [3/3] install Qt %QT_VERSION% (with webengine) ==
where python >nul 2>&1 || where python3 >nul 2>&1 || (
    echo   ERROR: python not found (aqtinstall needs it^) >&2
    exit /b 1
)
set "PY=python"
where %PY% >nul 2>&1 || set "PY=python3"

%PY% -m aqt version >nul 2>&1
if errorlevel 1 (
    echo   installing aqtinstall via pip...
    %PY% -m pip install aqtinstall
)

if exist "%QT_INSTALL_DIR%\bin\qmake.exe" (
    echo   Qt already installed at %QT_INSTALL_DIR% -- skipping
) else (
    echo   installing Qt %QT_VERSION% %QT_ARCH% with webengine and deps...
    %PY% -m aqt install-qt windows desktop %QT_VERSION% %QT_ARCH% -m qtwebengine qtwebsockets qtwebchannel qtpdf qtpositioning -O "%QT_PREFIX%"
    if not exist "%QT_INSTALL_DIR%\bin\qmake.exe" (
        echo   ERROR: Qt install dir not found at %QT_INSTALL_DIR% >&2
        exit /b 1
    )
)

echo.
echo == DONE ==
echo Qt installed at: %QT_INSTALL_DIR%
echo.
echo Next -- build:
echo   scripts\build-windows.bat
echo.
endlocal
