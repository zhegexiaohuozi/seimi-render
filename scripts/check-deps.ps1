# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# dist 自包含检查器：扫描 dist 内每个 .exe/.dll 的导入表，报告：
#   [MISS] 导入的 DLL 不在 dist 内（干净机器上缺）
#   [PATH] 导入用了路径形式（移动后失效）
#   [SYS]  忽略：OS/CRT DLL（Windows 总有）
# 用法: powershell -File scripts\check-deps.ps1 [-Dist path] [-Dumpbin path]
[CmdletBinding()]
param(
    [string]$Dist    = '',
    [string]$Dumpbin = ''
)

$ErrorActionPreference = 'Stop'

# resolve script dir（旧版 PS 的 -File 调用下 PSScriptRoot 为空）
if (-not $PSScriptRoot) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    $scriptRoot = $PSScriptRoot
}
if ([string]::IsNullOrWhiteSpace($Dist)) {
    $Dist = Join-Path $scriptRoot '..\build\dist\seimi-render-win-x64'
}

if (-not (Test-Path -LiteralPath $Dist)) {
    Write-Error "dist dir not found: $Dist"
    exit 1
}
$Dist = (Resolve-Path -LiteralPath $Dist).Path

# --- locate dumpbin ---
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin)) {
    $found = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($found) { $Dumpbin = $found.Source }
}
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        $vswhere = "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
        if ($vs) {
            $cand = Get-ChildItem -LiteralPath "$vs\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
                Where-Object { Test-Path -LiteralPath $_ } |
                Select-Object -First 1
            if ($cand) { $Dumpbin = $cand }
        }
    }
}
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin)) {
    Write-Error "dumpbin not found. Pass -Dumpbin or run from a Developer prompt."
    exit 1
}

Write-Host "== checking dist: $Dist"
Write-Host "   dumpbin   : $Dumpbin"

# --- dist 内所有 PE 文件（递归）---
$pes = Get-ChildItem -LiteralPath $Dist -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.exe', '.dll' }
$have = $pes | ForEach-Object { $_.Name }
Write-Host "   PE files  : $($pes.Count)"
Write-Host ""

# 忽略的系统/CRT DLL 前缀
$sysPrefix = @(
    'api-ms-','vcruntime','msvcp','ucrt','kernel32','user32','advapi32','ole32',
    'shell32','gdi32','ntdll','comctl32','shlwapi','winmm','ws2_32','oleaut32',
    'comdlg32','dwmapi','userenv','psapi','version','wininet','secur32','crypt32',
    'bcrypt','ncrypt','iphlpapi','dbghelp','powrprof','setupapi','d3d11','dxgi',
    'mf','mfplat','mfreadwrite','mfuuid','evr','propsys','oleacc','win32k',
    'sspicli','userenv','devobj','cfgmgr32','msvcp140_codecvt_ids','mscoree',
    'api_ms'
)

$missCount = 0
$pathCount = 0
$checkedCount = 0

foreach ($pe in $pes) {
    $checkedCount++
    $out = & $Dumpbin /dependents $pe.FullName 2>$null
    $inDeps = $false
    foreach ($line in $out) {
        $t = "$line".Trim()
        if ($t -match '^Image has the following dependencies') { $inDeps = $true; continue }
        if (-not $inDeps) { continue }
        if ([string]::IsNullOrWhiteSpace($t)) { $inDeps = $false; continue }
        if ($t -notmatch '\.dll$') { continue }

        $dep = $t

        # 路径形式（含 \ 或 :）→ 移动后失效
        if ($dep -match '[\\:]') {
            Write-Host ("  [PATH] {0} -> {1}" -f $pe.Name, $dep) -ForegroundColor Red
            $pathCount++
            continue
        }

        $bare = Split-Path $dep -Leaf

        $isSys = $false
        foreach ($p in $sysPrefix) {
            if ($bare -like "$p*") { $isSys = $true; break }
        }
        if ($isSys) { continue }

        if ($have -notcontains $bare) {
            Write-Host ("  [MISS] {0} -> {1}" -f $pe.Name, $bare) -ForegroundColor Yellow
            $missCount++
        }
    }
}

Write-Host ""
Write-Host "== summary =="
Write-Host "   checked PEs      : $checkedCount"
Write-Host "   path-form imports: $pathCount"
Write-Host "   missing imports  : $missCount"
if ($pathCount -eq 0 -and $missCount -eq 0) {
    Write-Host "   [OK] dist is self-contained for static imports." -ForegroundColor Green
    exit 0
} else {
    Write-Host "   [WARN] see issues above." -ForegroundColor Yellow
    exit 2
}
