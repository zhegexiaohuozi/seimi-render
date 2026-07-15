@echo off
REM =============================================================
REM seimi-render Windows build environment setup (Windows 10/11 x64)
REM
REM Does three things:
REM   1) Checks the MSVC toolchain and Windows SDK (VS Build Tools or full VS OK)
REM   2) Installs CMake and Ninja via winget (if missing)
REM   3) Installs a standalone Qt 6.7.2 (with qtwebengine etc.) into C:\Qt
REM
REM Prereq: "Visual Studio Build Tools 2022/2026" or full VS 2022 is installed,
REM         with "MSVC v143 - VS 2022 C++ x64/x86 build tools" and
REM         "Windows 10/11 SDK". The official Qt 6.7.x Windows binaries use the
REM         win64_msvc2019_64 ABI label but are binary-compatible with MSVC 2019/2022,
REM         so building our project with the v143 toolchain works fine.
REM
REM Usage (run in a normal cmd; no Developer Prompt needed):
REM   scripts\setup-windows.bat
REM
REM Env vars (optional):
REM   set QT_VERSION=6.7.2 && set QT_PREFIX=C:\Qt && scripts\setup-windows.bat
REM
REM Idempotent: already-installed tools/Qt are skipped.
REM =============================================================
setlocal enableextensions enabledelayedexpansion

if "%QT_VERSION%"=="" set "QT_VERSION=6.7.2"
if "%QT_PREFIX%"=="" set "QT_PREFIX=C:\Qt"
REM aqtinstall takes arch arg "win64_msvc2019_64" but installs to dir without
REM the "win64_" prefix, i.e. C:\Qt\6.7.2\msvc2019_64. Two vars for arg vs disk path.
if "%QT_ARCH%"=="" set "QT_ARCH=win64_msvc2019_64"
if "%QT_ARCH_DIR%"=="" set "QT_ARCH_DIR=msvc2019_64"
set "QT_INSTALL_DIR=%QT_PREFIX%\%QT_VERSION%\%QT_ARCH_DIR%"

echo == seimi-render Windows setup ==
echo   qt version : %QT_VERSION%
echo   qt arch    : %QT_ARCH%
echo   qt prefix  : %QT_PREFIX%

REM ---- 1. MSVC toolchain + Windows SDK ----
echo == [1/3] locate MSVC toolchain ==
REM NOTE: %ProgramFiles(x86)% has parens that break cmd parsing; use the call trick.
call set "PF86=%%ProgramFiles(x86)%%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo   ERROR: vswhere.exe not found. Install Visual Studio 2022 or Build Tools first. >&2
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if "%VSINSTALL%"=="" (
    echo   ERROR: no VS install with MSVC C++ tools found. >&2
    echo          In VS Installer, enable the "Desktop development with C++" workload, >&2
    echo          including "MSVC v143 - VS 2022 C++ x64/x86 build tools" and "Windows SDK". >&2
    exit /b 1
)
echo   VS install: %VSINSTALL%
REM Test vcvarsall presence without expanding the (x86)-bearing path inside
REM an if-block (parens in the path break cmd parsing). Use a flag var.
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

REM ---- 3. aqtinstall + Qt 6.7.x ----
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
echo Next -- build (the script sets up MSVC env itself, no Developer Prompt needed):
echo   scripts\build-windows.bat
echo.
endlocal
