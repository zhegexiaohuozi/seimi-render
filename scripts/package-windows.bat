REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
REM seimi-render Windows 自包含打包（对标 macOS package.sh / Linux package-linux.sh）
REM windeployqt 自动拷 Qt6*.dll + WebEngine 资源 + QtWebEngineProcess.exe + 平台插件并修正引用。
REM 用法: scripts\package-windows.bat [clean] [Release|Debug]
REM 环境变量: QT_PREFIX, QT_VERSION, QT_ARCH_DIR, DIST_NAME
setlocal enableextensions enabledelayedexpansion

REM ---- 解析参数 ----
REM %~dp0 必须在 shift 前捕获：shift 后 %0 退化成裸参数，%~dp0 指向调用方 cwd。
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
set "BUILD_DIR=%ROOT_DIR%\build"

REM 产物名带版本 + 架构（对齐 macOS / Linux package 脚本）。
REM 版本从 CMakeLists.txt 的 project(VERSION) 读；架构从 QT_ARCH_DIR 提取（_64->x64, _32->x86）。
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

REM ---- 0. (optional) clean ----
if "%DO_CLEAN%"=="1" (
    echo == [0/3] deep clean build dir ==
    if exist "%BUILD_DIR%" (
        del "\\?\%BUILD_DIR%\nul" 2>nul
        rmdir /s /q "%BUILD_DIR%"
        if exist "%BUILD_DIR%" (
            echo ERROR: rmdir failed - build dir locked ^(running exe/explorer/IDE?^) >&2
            exit /b 1
        )
        echo   build dir removed.
    ) else (
        echo   build dir absent, nothing to clean.
    )
)

REM ---- 1. build ----
echo == [1/3] build exe ==
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

REM ---- 2. windeployqt ----
echo == [2/3] windeployqt (bundle Qt + WebEngine) ==
set "WINDEPLOYQT=%QT_INSTALL_DIR%\bin\windeployqt.exe"
if not exist "%WINDEPLOYQT%" (
    echo ERROR: windeployqt not found at %WINDEPLOYQT% >&2
    exit /b 1
)

if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%" >nul 2>&1
copy /y "%BIN_SRC%" "%DIST_DIR%\seimi-render.exe" >nul
copy /y "%SCRIPT_DIR%start-windows.bat" "%DIST_DIR%\启动服务.bat" >nul

if exist "%ROOT_DIR%\admin-ui" (
    xcopy /y /e /i "%ROOT_DIR%\admin-ui" "%DIST_DIR%\admin-ui\" >nul
)

REM third_party JS（运行时从磁盘加载，CMake install 规则不覆盖 windeployqt dist 目录）
if exist "%ROOT_DIR%\third_party\readability" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\readability" "%DIST_DIR%\third_party\readability\" >nul
)
if exist "%ROOT_DIR%\third_party\stealth" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\stealth" "%DIST_DIR%\third_party\stealth\" >nul
)
if exist "%ROOT_DIR%\third_party\serp" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\serp" "%DIST_DIR%\third_party\serp\" >nul
)

REM CJK 字体（Qt 不再带字体，WebEngine 找不到字体目录会警告且中文渲染成方块）
set "FONT_SRC=%SystemRoot%\Fonts\msyh.ttc"
if not exist "%FONT_SRC%" set "FONT_SRC=%SystemRoot%\Fonts\msyh.ttf"
if exist "%FONT_SRC%" (
    if not exist "%DIST_DIR%\lib\fonts" mkdir "%DIST_DIR%\lib\fonts"
    copy /y "%FONT_SRC%" "%DIST_DIR%\lib\fonts\msyh.ttc" >nul 2>&1
)

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

REM offscreen 插件：windeployqt 只部署链接期可见插件，offscreen 运行时按 QT_QPA_PLATFORM 选择。
if not exist "%DIST_DIR%\plugins\platforms\qoffscreen.dll" (
    if exist "%QT_INSTALL_DIR%\plugins\platforms\qoffscreen.dll" (
        echo   [fixup] copy offscreen platform plugin
        if not exist "%DIST_DIR%\plugins\platforms" mkdir "%DIST_DIR%\plugins\platforms"
        copy /y "%QT_INSTALL_DIR%\plugins\platforms\qoffscreen.dll" "%DIST_DIR%\plugins\platforms\" >nul
    ) else (
        echo   [WARN] offscreen plugin missing; headless render will fail >&2
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
echo   (or: %DIST_NAME%\seimi-render.exe --http-port 8088 --ws-port 8089)
endlocal
