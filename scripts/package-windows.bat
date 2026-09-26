REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
setlocal enableextensions enabledelayedexpansion

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
if "%QT_INSTALL_DIR%"=="" set "QT_INSTALL_DIR=%QT_PREFIX%\%QT_VERSION%\%QT_ARCH_DIR%"
set "BUILD_DIR=%ROOT_DIR%\build"

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

if "%DO_CLEAN%"=="1" (
    echo == [0/4] deep clean build dir ==
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

echo == [1/4] build exe ==
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

echo == [2/4] windeployqt (bundle Qt + WebEngine + QML modules) ==
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

if exist "%ROOT_DIR%\third_party\readability" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\readability" "%DIST_DIR%\third_party\readability\" >nul
)
if not exist "%DIST_DIR%\third_party\markdown" mkdir "%DIST_DIR%\third_party\markdown"
for %%F in (convert.js turndown.js turndown-plugin-gfm.js LICENSE.turndown LICENSE.turndown-plugin-gfm) do (
    copy /y "%ROOT_DIR%\third_party\markdown\%%F" "%DIST_DIR%\third_party\markdown\" >nul
    if errorlevel 1 exit /b 1
)
if exist "%ROOT_DIR%\third_party\stealth" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\stealth" "%DIST_DIR%\third_party\stealth\" >nul
)
if exist "%ROOT_DIR%\third_party\serp" (
    xcopy /y /e /i "%ROOT_DIR%\third_party\serp" "%DIST_DIR%\third_party\serp\" >nul
)

REM CJK 字体（Qt 不带字体，WebEngine 找不到字体目录会警告且中文渲染成方块）
set "FONT_SRC=%SystemRoot%\Fonts\msyh.ttc"
if not exist "%FONT_SRC%" set "FONT_SRC=%SystemRoot%\Fonts\msyh.ttf"
if exist "%FONT_SRC%" (
    if not exist "%DIST_DIR%\lib\fonts" mkdir "%DIST_DIR%\lib\fonts"
    copy /y "%FONT_SRC%" "%DIST_DIR%\lib\fonts\msyh.ttc" >nul 2>&1
)

REM --qmldir：扫描 qml/ 的 import（QtQuick.Controls/Layouts 等），把 QML 模块连同
REM 插件部署进 dist\qml\ 并生成 qt.conf（GUI 模式运行时按它解析）。不能带
REM --no-quick-import（会关掉扫描 → 分发版 --gui 白屏）。
REM --include-plugins qoffscreen：无头服务运行时按 QT_QPA_PLATFORM=offscreen 选平台，
REM 链接期不可见，windeployqt 默认不部署（漏了 = 分发版无头启动 qFatal：
REM "Could not find the Qt platform plugin offscreen"）。windeployqt 的插件目录是
REM 顶层 platforms\（无 plugins\ 前缀、无 qt.conf 重定向），下方兜底拷贝同步该路径。
set "DEPLOY_FLAGS=--qmldir "%ROOT_DIR%\qml" --include-plugins qoffscreen --no-system-d3d-compiler --no-opengl-sw"
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

REM offscreen 插件兜底：windeployqt 把平台插件部署在顶层 platforms\（不是 plugins\platforms\）。
if not exist "%DIST_DIR%\platforms\qoffscreen.dll" (
    if exist "%QT_INSTALL_DIR%\plugins\platforms\qoffscreen.dll" (
        echo   [fixup] copy offscreen platform plugin
        if not exist "%DIST_DIR%\platforms" mkdir "%DIST_DIR%\platforms"
        copy /y "%QT_INSTALL_DIR%\plugins\platforms\qoffscreen.dll" "%DIST_DIR%\platforms\" >nul
    ) else (
        echo   [WARN] offscreen plugin missing; headless render will fail >&2
    )
)

echo == [3/4] verify and zip ==
echo   -- verify bundle --
if exist "%DIST_DIR%\seimi-render.exe"                 (echo   [OK]   main exe)             else (echo   [MISS] main exe)
if exist "%DIST_DIR%\Qt6WebEngineCore.dll"             (echo   [OK]   Qt6WebEngineCore.dll) else (echo   [MISS] Qt6WebEngineCore.dll)
if exist "%DIST_DIR%\resources\icudtl.dat"             (echo   [OK]   icudtl.dat)           else (echo   [MISS] icudtl.dat)
if exist "%DIST_DIR%\platforms\qoffscreen.dll"      (echo   [OK]   offscreen plugin)     else (echo   [MISS] offscreen plugin)
if exist "%DIST_DIR%\Qt6Quick.dll"                     (echo   [OK]   Qt6Quick.dll)         else (echo   [MISS] Qt6Quick.dll)
if exist "%DIST_DIR%\qml\QtQuick\Controls"             (echo   [OK]   qml QtQuick.Controls) else (echo   [MISS] qml QtQuick.Controls --gui 会白屏)
dir /b /s "%DIST_DIR%\QtWebEngineProcess.exe" >nul 2>&1 && (echo   [OK]   QtWebEngineProcess.exe) || (echo   [MISS] QtWebEngineProcess.exe)

set "ZIP_PATH=%BUILD_DIR%\%DIST_NAME%.zip"
if exist "%ZIP_PATH%" del "%ZIP_PATH%"
powershell -NoProfile -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%ZIP_PATH%' -Force"
if exist "%ZIP_PATH%" (
    for %%I in ("%ZIP_PATH%") do echo   zip: %ZIP_PATH% (%%~zI bytes)
) else (
    echo   [WARN] zip not produced
)

REM ---- [4/4] NSIS 安装器（可选：本机装有 NSIS 才产出；无则跳过不影响 zip）----
REM 安装器把 DIST_DIR 整树装进 %LOCALAPPDATA%\seimi-render（per-user，免 UAC），
REM 并写 .seimi-install-manifest 标记——GUI 更新管理器据此识别「安装版」，
REM 走 NSIS /S 静默重装而非绿色版目录交换。
echo == [4/4] NSIS installer ==
set "SETUP_PATH=%BUILD_DIR%\%DIST_NAME%-setup.exe"
where makensis >nul 2>&1
if errorlevel 1 (
    echo   [SKIP] makensis not found in PATH; installer not built ^(zip is still complete^)
    echo          install NSIS ^(https://nsis.sourceforge.io^) and re-run to get the setup exe.
) else (
    makensis -V2 -DAPP_VERSION="%APP_VERSION%" -DSRC_DIR="%DIST_DIR%" -DOUT_FILE="%SETUP_PATH%" "%SCRIPT_DIR%installer-windows.nsi"
    if errorlevel 1 (
        echo   [WARN] makensis failed; installer not produced >&2
    ) else if exist "%SETUP_PATH%" (
        for %%I in ("%SETUP_PATH%") do echo   setup: %SETUP_PATH% (%%~zI bytes)
    ) else (
        echo   [WARN] installer not produced >&2
    )
)

echo.
echo == DONE ==
echo   bundle : %DIST_DIR%
echo   zip    : %ZIP_PATH%
if exist "%SETUP_PATH%" echo   setup  : %SETUP_PATH%
echo.
echo   on target machine, unzip and double-click:
echo     %DIST_NAME%\启动服务.bat        (headless server)
echo   or run GUI console:
echo     %DIST_NAME%\seimi-render.exe --gui
echo   or use the NSIS setup exe ^(per-user install, start-menu shortcut^).
endlocal
