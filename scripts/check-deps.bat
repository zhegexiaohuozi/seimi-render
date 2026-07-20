REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
REM dist 自包含检查器：扫描 dist 内每个 .exe/.dll 的导入表，标记：
REM   [MISS] 导入的 DLL 不在 dist 内（干净机器上会缺）
REM   [PATH] 导入用了路径形式（拷贝/移动后失效）
REM   系统 DLL（api-ms/vc runtime/win32）忽略
REM 用法: scripts\check-deps.bat [dist_dir]  默认 build\dist\seimi-render-win-x64
setlocal enabledelayedexpansion

set "DIST=%~1"
if "%DIST%"=="" set "DIST=%~dp0..\build\dist\seimi-render-win-x64"
if not exist "%DIST%" (
    echo ERROR: dist dir not found: %DIST% >&2
    exit /b 1
)

REM ---- 定位 dumpbin: 环境变量 > PATH > vswhere（全 goto 流程）----
if not "%DUMPBIN%"=="" if exist "%DUMPBIN%" goto have_dumpbin
set "DUMPBIN="
for /f "delims=" %%i in ('where dumpbin 2^>nul') do set "DUMPBIN=%%i"
if not "!DUMPBIN!"=="" goto have_dumpbin
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_dumpbin
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
if "%VSINSTALL%"=="" goto no_dumpbin
set "DUMPBIN="
for /f "delims=" %%i in ('where /r "%VSINSTALL%" dumpbin.exe 2^>nul') do (
    echo %%i | findstr /i "Hostx64\\x64" >nul && set "DUMPBIN=%%i"
)
if "!DUMPBIN!"=="" goto no_dumpbin
goto have_dumpbin

:no_dumpbin
echo ERROR: dumpbin not found. set DUMPBIN=... or run from Developer Prompt. >&2
exit /b 1

:have_dumpbin
REM 折叠成 8.3 短路径：MSVC 在 "Program Files (x86)" 下，括号会破坏 for/f。
for %%X in ("%DUMPBIN%") do set "DUMPBIN=%%~sX"

echo == checking dist: %DIST%
echo    dumpbin   : %DUMPBIN%

REM ---- "have" 列表: dist 内所有 PE 文件名（递归）----
set "HAVE="
set "PE_COUNT=0"
for /r "%DIST%" %%f in (*.exe *.dll) do (
    set "HAVE=!HAVE! %%~nxf"
    set /a PE_COUNT+=1
)
echo    PE files  : %PE_COUNT%
echo.

REM 一个 dumpbin /dependents 写临时文件，findstr 扫两遍。避免 for/f 带管道（cmd 会
REM 搞坏引号）和嵌套 for 循环（会卡 5 分钟）。dumpbin ~30ms/次，47 个 PE ~1.5s。
set "MISS_COUNT=0"
set "PATH_COUNT=0"
set "TMPFILE=%TEMP%\seimi_dep.txt"

for /r "%DIST%" %%f in (*.exe *.dll) do (
    set "PE_NAME=%%~nxf"
    "%DUMPBIN%" /dependents "%%f" > "%TMPFILE%" 2>nul
    REM 不用 /b：dumpbin 的导入行有缩进，行首匹配不到。子串匹配系统 DLL 名即可。
    for /f "delims=" %%D in ('findstr /i /r "\.dll$" "%TMPFILE%" ^| findstr /i /v "api-ms- vcruntime msvcp ucrt kernel32 user32 advapi32 ole32 shell32 gdi32 ntdll comctl32 shlwapi winmm ws2_32 oleaut32 comdlg32 dwmapi userenv psapi version wininet secur32 crypt32 bcrypt iphlpapi dbghelp powrprof setupapi d3d11 d3d9 d3d12 dxgi mfplat mfreadwrite mfuuid propsys oleacc netapi32 dnsapi winhttp urlmon dwrite imm32 winusb dhcpcsvc ncrypt cfgmgr wtsapi32 pdh uxtheme authz mpr hid.dll mf.dll evr.dll dump file type image section summary contains"') do (
        set "DEP=%%D"
        for /f "tokens=* delims= " %%t in ("!DEP!") do set "DEP=%%t"
        echo !DEP! | findstr /r "[\\:]" >nul
        if not errorlevel 1 (
            echo   [PATH] !PE_NAME! -^> !DEP!
            set /a PATH_COUNT+=1
        ) else (
            set "FOUND=0"
            for %%H in (!HAVE!) do (
                if /i "%%H"=="!DEP!" set "FOUND=1"
            )
            if "!FOUND!"=="0" (
                echo   [MISS] !PE_NAME! -^> !DEP!
                set /a MISS_COUNT+=1
            )
        )
    )
)
del "%TMPFILE%" >nul 2>&1

echo.
echo == summary ==
echo    path-form imports : !PATH_COUNT!
echo    missing imports   : !MISS_COUNT!
if "!PATH_COUNT!"=="0" if "!MISS_COUNT!"=="0" (
    echo    [OK] dist self-contained for static imports.
) else (
    echo    [WARN] issues listed above.
)
endlocal
