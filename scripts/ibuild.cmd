@echo off
rem Incremental build for dev - keeps build dir (build-windows.bat wipes it each run).
rem Usage: scripts\ibuild.cmd [ninja args]
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cmake --build build %*
exit /b %errorlevel%
