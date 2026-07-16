REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
REM =============================================================
REM seimi-render Windows self-contained packaging script
REM
REM Mirrors macOS package.sh / Linux package-linux.sh: produces a
REM redistributable self-contained folder (all Qt DLLs + WebEngine
REM resources + QtWebEngineProcess.exe), so the target machine needs
REM no Qt install -- just unzip and run.
REM
REM How it works: windeployqt automatically copies Qt6*.dll, WebEngine
REM resources (resources/, translations/qtwebengine_locales/),
REM QtWebEngineProcess.exe subprocess, and platform plugins (qoffscreen.dll
REM etc.) into the target folder and fixes up references. Much simpler
REM than the Linux manual-bundle + patchelf approach.
REM
REM Usage:
REM   scripts\package-windows.bat          REM default Release
REM   scripts\package-windows.bat Debug
REM   scripts\package-windows.bat clean         REM 彻底清理 build 目录后重新打包（Release）
REM   scripts\package-windows.bat clean Debug   REM clean 可与配置组合，顺序不限
REM
REM Env vars (optional): QT_PREFIX, QT_VERSION, DIST_NAME
REM =============================================================
setlocal enableextensions enabledelayedexpansion

REM ---- 解析参数：支持 clean（可选）+ CONFIG（可选），顺序不限 ----
REM   不带参数           默认 Release，常规清理
REM   Debug              Debug 配置
REM   clean              彻底清理 build 目录后重新打包（默认 Release）
REM   clean + Debug      clean 可与配置组合，顺序不限
REM 关键：%~dp0 必须在 shift() 之前捕获。下面的 parse 循环会对全部形参
REM （含 %0）执行 shift()，shift 之后 %0 变成裸参数（如 "clean"），%~dp0 会
REM 退化为调用方的 cwd，导致定位同目录脚本失败（实测：传 clean 后找不到
REM build-windows.bat）。SCRIPT_DIR/ROOT_DIR 在此处钉死，后续一律用变量。
set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%~dp0.."

set "CONFIG="
set "DO_CLEAN=0"
:parse_args
if "%~1"=="" goto args_done
set "ARG=%~1"
if /i "%ARG%"=="clean" (
    set "DO_CLEAN=1"
) else if /i "%ARG%"=="Release" (
    set "CONFIG=Release"
) else if /i "%ARG%"=="Debug" (
    set "CONFIG=Debug"
) else (
    echo ERROR: unknown argument "%ARG%" ^(expected: clean ^| Release ^| Debug^) >&2
    exit /b 1
)
shift
goto parse_args
:args_done
if "%CONFIG%"=="" set "CONFIG=Release"

if "%QT_PREFIX%"=="" set "QT_PREFIX=C:\Qt"
if "%QT_VERSION%"=="" set "QT_VERSION=6.7.2"
if "%QT_ARCH_DIR%"=="" set "QT_ARCH_DIR=msvc2019_64"
set "QT_INSTALL_DIR=%QT_PREFIX%\%QT_VERSION%\%QT_ARCH_DIR%"
REM SCRIPT_DIR/ROOT_DIR 已在 shift 循环之前钉死，这里直接复用变量，
REM 不再使用 %~dp0（shift 后已退化指向调用方 cwd）。
set "BUILD_DIR=%ROOT_DIR%\build"

REM 产物名带版本 + 架构（对齐 macOS package.sh / Linux package-linux.sh）。
REM 版本从 CMakeLists.txt 的 project(VERSION) 读；架构从 QT_ARCH_DIR 提取
REM （_64 -> x64, _32 -> x86），对齐 Linux 从二进制读架构的思路。
REM findstr 精确匹配缩进的 "    VERSION "（4 空格），避开 cmake_minimum_required 行。
set "APP_VERSION=unknown"
for /f "tokens=2" %%v in ('findstr /b /c:"    VERSION " "%ROOT_DIR%\CMakeLists.txt" 2^>nul') do (
    set "APP_VERSION=%%v"
)
set "WIN_ARCH=x64"
if /i "%QT_ARCH_DIR:~-3%"=="_32" set "WIN_ARCH=x86"
set "DIST_NAME=seimi-render-%APP_VERSION%-win-%WIN_ARCH%"
set "DIST_DIR=%BUILD_DIR%\dist\%DIST_NAME%"

echo == seimi-render packaging (Windows) ==
echo   root      : %ROOT_DIR%
echo   config    : %CONFIG%
echo   qt prefix : %QT_INSTALL_DIR%
echo   dist dir  : %DIST_DIR%

REM ---- 0. (optional) deep clean build dir before anything else ----
REM clean 比 build-windows.bat 的常规 robocopy /MIR 更彻底: 直接 rmdir 整个 build
REM 目录(含 dist/、旧 zip、CMake 缓存、所有中间产物), 强制从零重建。
REM 在 build-windows.bat 转调之前做, 确保后者进来时 build 不存在 -> mkdir -> 全量 configure。
if "%DO_CLEAN%"=="1" (
    echo == [0/3] deep clean build dir ==
    if exist "%BUILD_DIR%" (
        REM 先处理可能残留的 "nul" 设备名文件(> nul 重定向误产生), 用 \\?\ 前缀绕过保留名
        del "\\?\%BUILD_DIR%\nul" 2>nul
        rmdir /s /q "%BUILD_DIR%"
        if exist "%BUILD_DIR%" (
            echo ERROR: rmdir failed - build dir still exists, possibly locked by a process >&2
            echo        e.g. seimi-render.exe running, file explorer, or IDE. Close it and retry. >&2
            exit /b 1
        )
        echo   build dir removed.
    ) else (
        echo   build dir absent, nothing to clean.
    )
)

REM ---- 1. build the exe first (reuse build-windows.bat) ----
echo == [1/3] build exe ==
REM 用 shift 循环前钉死的 SCRIPT_DIR 定位同目录脚本，不用 %~dp0（已退化）。
call "%SCRIPT_DIR%build-windows.bat" %CONFIG%
if errorlevel 1 (
    echo ERROR: build failed >&2
    exit /b 1
)

set "BIN_SRC=%BUILD_DIR%\seimi-render.exe"
if not exist "%BIN_SRC%" (
    echo ERROR: binary not found at %BIN_SRC% >&2
    exit /b 1
)

REM ---- 2. windeployqt self-contained deployment ----
echo == [2/3] windeployqt (bundle Qt + WebEngine) ==
set "WINDEPLOYQT=%QT_INSTALL_DIR%\bin\windeployqt.exe"
if not exist "%WINDEPLOYQT%" (
    echo ERROR: windeployqt not found at %WINDEPLOYQT% >&2
    exit /b 1
)

REM prepare dist dir, copy main exe in
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%" >nul 2>&1
copy /y "%BIN_SRC%" "%DIST_DIR%\seimi-render.exe" >nul

REM 双击即用的快速启动脚本（与 exe 同目录，确保相对路径资源可解析）
copy /y "%SCRIPT_DIR%start-windows.bat" "%DIST_DIR%\启动服务.bat" >nul

REM admin UI static resources (bundled with binary)
if exist "%ROOT_DIR%\admin-ui" (
    xcopy /y /e /i "%ROOT_DIR%\admin-ui" "%DIST_DIR%\admin-ui\" >nul
)

REM third_party JS resources (readability/stealth/serp) bundled with binary.
REM These are loaded from disk at runtime by RenderPool (extract.js/simplify.js/
REM stealth.js/baidu_serp.js). CMake install rules don't apply to the windeployqt
REM dist folder, so copy them explicitly alongside the binary.
if exist "%ROOT_DIR%\third_party\readability" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\readability" "%DIST_DIR%\third_party\readability\" >nul
)
if exist "%ROOT_DIR%\third_party\stealth" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\stealth" "%DIST_DIR%\third_party\stealth\" >nul
)
if exist "%ROOT_DIR%\third_party\serp" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\serp" "%DIST_DIR%\third_party\serp\" >nul
)

REM fonts: Qt ships no fonts anymore; WebEngine warns "Cannot find font directory"
REM and Chinese pages may render as boxes. Bundle a CJK font (Microsoft YaHei) so
REM the target machine needs no extra fonts and the warning is gone.
set "FONT_SRC=%SystemRoot%\Fonts\msyh.ttc"
if not exist "%FONT_SRC%" set "FONT_SRC=%SystemRoot%\Fonts\msyh.ttf"
if exist "%FONT_SRC%" (
    if not exist "%DIST_DIR%\lib\fonts" mkdir "%DIST_DIR%\lib\fonts"
    copy /y "%FONT_SRC%" "%DIST_DIR%\lib\fonts\msyh.ttc" >nul 2>&1
)

REM run windeployqt against the exe in the dist dir; it collects all deps
set "DEPLOY_FLAGS=--no-quick-import --no-system-d3d-compiler --no-opengl-sw"
if /i "%CONFIG%"=="Debug" (
    set "DEPLOY_FLAGS=%DEPLOY_FLAGS% --debug"
) else (
    set "DEPLOY_FLAGS=%DEPLOY_FLAGS% --release"
)

"%WINDEPLOYQT%" "%DIST_DIR%\seimi-render.exe" %DEPLOY_FLAGS% --verbose 1
if errorlevel 1 (
    echo ERROR: windeployqt failed >&2
    exit /b 1
)

REM ensure the offscreen platform plugin is bundled (windeployqt deploys only
REM link-time-visible plugins by default; offscreen is chosen at runtime via
REM QT_QPA_PLATFORM, so make sure platforms\qoffscreen.dll is present).
if not exist "%DIST_DIR%\plugins\platforms\qoffscreen.dll" (
    if exist "%QT_INSTALL_DIR%\plugins\platforms\qoffscreen.dll" (
        echo   [fixup] copy offscreen platform plugin
        if not exist "%DIST_DIR%\plugins\platforms" mkdir "%DIST_DIR%\plugins\platforms"
        copy /y "%QT_INSTALL_DIR%\plugins\platforms\qoffscreen.dll" "%DIST_DIR%\plugins\platforms\" >nul
    ) else (
        echo   [WARN] offscreen plugin missing even in Qt install; headless render will fail >&2
    )
)

REM ---- 3. verify + zip ----
echo == [3/3] verify and zip ==
echo   -- verify bundle --
if exist "%DIST_DIR%\seimi-render.exe"                 (echo   [OK]   main exe)             else (echo   [MISS] main exe)
if exist "%DIST_DIR%\Qt6WebEngineCore.dll"             (echo   [OK]   Qt6WebEngineCore.dll) else (echo   [MISS] Qt6WebEngineCore.dll)
if exist "%DIST_DIR%\resources\icudtl.dat"             (echo   [OK]   icudtl.dat)           else (echo   [MISS] icudtl.dat)
if exist "%DIST_DIR%\plugins\platforms\qoffscreen.dll" (echo   [OK]   offscreen plugin)     else (echo   [MISS] offscreen plugin)
dir /b /s "%DIST_DIR%\QtWebEngineProcess.exe" >nul 2>&1 && (echo   [OK]   QtWebEngineProcess.exe) || (echo   [MISS] QtWebEngineProcess.exe)

REM zip via PowerShell Compress-Archive (bundled with Windows)
set "ZIP_PATH=%BUILD_DIR%\%DIST_NAME%.zip"
if exist "%ZIP_PATH%" del "%ZIP_PATH%"
powershell -NoProfile -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%ZIP_PATH%' -Force"
if exist "%ZIP_PATH%" (
    for %%I in ("%ZIP_PATH%") do echo   zip: %ZIP_PATH% (%%~zI bytes)
) else (
    echo   [WARN] zip not produced
)

echo.
echo == DONE ==
echo   bundle : %DIST_DIR%
echo   zip    : %ZIP_PATH%
echo.
echo   on target machine, unzip and double-click:
echo     %DIST_NAME%\启动服务.bat
echo   (or run manually: %DIST_NAME%\seimi-render.exe --http-port 8088 --ws-port 8089)
endlocal
