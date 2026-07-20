REM SPDX-FileCopyrightText: 2026 wanghaomiao.cn
REM SPDX-License-Identifier: Apache-2.0

@echo off
cd /d "%~dp0"
title seimi-render console service

echo   seimi-render service started
echo.
echo   Admin UI / HTTP : http://127.0.0.1:8088
echo   WebSocket       : ws://127.0.0.1:8089
echo   MCP  (Agent)    : http://127.0.0.1:8090/mcp
echo.
echo   Close this window or press Ctrl+C to stop.
echo.

seimi-render.exe --http-port 8088 --ws-port 8089 --mcp-port 8090

echo.
echo   seimi-render stopped (exit code %errorlevel%)
pause
