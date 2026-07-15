@echo off
REM =============================================================
REM Dist self-containment checker for seimi-render-win-x64
REM
REM Scans every .exe/.dll in the dist dir, flags:
REM   [MISS]  imported DLL not present anywhere in dist (breaks on clean machine)
REM   [PATH]  import is in path form (absolute/relative) -- breaks on copy
REM   system DLLs (api-ms / vc runtime / win32) are ignored
REM
REM Usage:
REM   scripts\check-deps.bat [dist_dir]
REM   set DUMPBIN=C:\path\to\dumpbin.exe && scripts\check-deps.bat
REM Default dist: build\dist\seimi-render-win-x64
REM =============================================================
setlocal enabledelayedexpansion

set "DIST=%~1"
if "%DIST%"=="" set "DIST=%~dp0..\build\dist\seimi-render-win-x64"
if not exist "%DIST%" (
    echo ERROR: dist dir not found: %DIST% >&2
    exit /b 1
)

REM ---- locate dumpbin: env > PATH > vswhere (all top-level, goto flow) ----
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
echo ERROR: dumpbin not found. >&2
echo        set DUMPBIN=C:\path\to\dumpbin.exe, or run from Developer Prompt. >&2
exit /b 1

:have_dumpbin
REM Collapse to 8.3 short path (MSVC lives under "Program Files (x86)" --
REM the literal '(' breaks for/f command strings).
for %%X in ("%DUMPBIN%") do set "DUMPBIN=%%~sX"

echo == checking dist: %DIST%
echo    dumpbin   : %DUMPBIN%

REM ---- "have" list: lowercase PE filenames present in dist (recursive) ----
set "HAVE="
set "PE_COUNT=0"
for /r "%DIST%" %%f in (*.exe *.dll) do (
    set "HAVE=!HAVE! %%~nxf"
    set /a PE_COUNT+=1
)
echo    PE files  : %PE_COUNT%
echo.

REM =============================================================
REM Plan: one dumpbin /dependents per PE -> temp file, then findstr the
REM temp file twice in one shot. Avoids for/f-with-pipe (cmd mangles the
REM pipe inside the quoted command string) and avoids nested for-loops
REM (the 5-min hang). dumpbin is ~30ms per call, so 47 PEs ~= 1.5s.
REM =============================================================
set "MISS_COUNT=0"
set "PATH_COUNT=0"
set "TMPFILE=%TEMP%\seimi_dep.txt"

for /r "%DIST%" %%f in (*.exe *.dll) do (
    set "PE_NAME=%%~nxf"
    REM dump imports to temp (overwrite each iteration)
    "%DUMPBIN%" /dependents "%%f" > "%TMPFILE%" 2>nul
    REM keep only *.dll lines, drop system DLLs + dumpbin headers.
    REM NOTE: NO /b here -- dumpbin indents import lines with spaces, so
   REM /b (begin-of-line) matches nothing. Plain substring match catches
    REM system DLLs regardless of indentation/case. System names are unique
    REM enough that substring false-positives are not a concern.
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
