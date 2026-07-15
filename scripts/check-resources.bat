@echo off
REM Runtime resources not covered by import-table scan (Qt/Chromium
REM load these dynamically via LoadLibrary / file paths at runtime, so
REM dumpbin /dependents can't see them -- windeployqt copies them by convention).
setlocal enabledelayedexpansion
set "DIST=%~1"
if "%DIST%"=="" set "DIST=%~dp0..\build\dist\seimi-render-win-x64"
cd /d "%DIST%" || exit /b 1

echo === WebEngine runtime resources ===
REM Qt 6.7+ uses *_100p.pak / *_200p.pak (older was *_100_percent.pak)
for %%R in (
    resources\icudtl.dat
    resources\qtwebengine_resources.pak
    resources\qtwebengine_resources_100p.pak
    resources\qtwebengine_resources_200p.pak
) do (
    if exist "%%R" (echo   [OK]   %%R) else (echo   [MISS] %%R)
)

echo === locales ===
set /a N=0
for %%L in (translations\qtwebengine_locales\*.pak) do set /a N+=1
echo   locale .pak count: !N!

echo === QtWebEngineProcess ===
for %%P in (QtWebEngineProcess.exe) do (
    if exist "%%P" (echo   [OK]   %%P) else (echo   [MISS] %%P)
)

echo === offscreen platform plugin ===
if exist plugins\platforms\qoffscreen.dll (echo   [OK]   offscreen plugin) else (echo   [MISS] offscreen plugin)

echo === CJK font ===
if exist lib\fonts\msyh.ttc (echo   [OK]   msyh.ttc) else (echo   [MISS] font)

echo === bundled JS resources ===
if exist third_party\readability\extract.js (echo   [OK]   readability) else (echo   [MISS] readability)
if exist third_party\stealth\stealth.js     (echo   [OK]   stealth)    else (echo   [MISS] stealth)
if exist third_party\serp\baidu_serp.js     (echo   [OK]   serp)       else (echo   [MISS] serp)

echo === admin-ui ===
if exist admin-ui\index.html (echo   [OK]   admin-ui) else (echo   [MISS] admin-ui)

endlocal
