# utils/build_slave.ps1 — STC8G1K08A 从机固件构建（Keil C51 / UV4 命令行）
#
# 用法：
#   .\utils\build_slave.ps1          增量编译
#   .\utils\build_slave.ps1 -Clean   清理后完整重建
#
# 产出：
#   src\slave\Objects\slave.hex   可直接用 STC-ISP 烧录
#
# 说明：
#   STC8G1K08A 用 Keil C51 工具链（AiCube 生成工程），不用 SDCC。
#   UV4 退出码：0=成功  2=编译错误  15=静默失败（几乎总是 .uvproj 带 BOM）

[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root 'src\slave'
$uv4  = 'C:\Keil_v5\UV4\UV4.exe'
$log  = Join-Path $env:TEMP 'slave_build.log'

if (-not (Test-Path $uv4)) {
    Write-Host "[ERR ] 找不到 Keil UV4: $uv4" -ForegroundColor Red
    exit 1
}

# 先确认 .uvproj 没有 UTF-8 BOM（BOM 会导致 UV4 静默 Exit=15）
$uvproj = Join-Path $proj 'slave.uvproj'
$head = [System.IO.File]::ReadAllBytes($uvproj)[0..2]
if ($head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF) {
    Write-Host '[WARN] slave.uvproj 带 UTF-8 BOM，UV4 会静默失败，正在移除...' -ForegroundColor Yellow
    $text = [System.IO.File]::ReadAllText($uvproj)
    [System.IO.File]::WriteAllText($uvproj, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host '[ OK ] BOM 已移除' -ForegroundColor Green
}

if ($Clean) {
    $mode = '-r'
    Write-Host '[MODE] 完整重建' -ForegroundColor Cyan
} else {
    $mode = '-b'
    Write-Host '[MODE] 增量编译' -ForegroundColor Cyan
}

if (Test-Path $log) { Remove-Item $log -Force }

$proc = Start-Process -FilePath $uv4 `
    -ArgumentList $mode, 'slave.uvproj', '-j0', '-o', $log `
    -WorkingDirectory $proj -Wait -PassThru

$code = $proc.ExitCode

if (Test-Path $log) {
    Get-Content $log -Encoding Default | ForEach-Object { Write-Host $_ }
}

switch ($code) {
    0  { Write-Host "[ OK ] 构建成功 (Exit=0)" -ForegroundColor Green }
    2  { Write-Host '[FAIL] 编译错误 (Exit=2)，见上方日志' -ForegroundColor Red }
    15 { Write-Host '[FAIL] UV4 静默失败 (Exit=15)：通常是 .uvproj 编码/BOM 问题' -ForegroundColor Red }
    default { Write-Host "[FAIL] UV4 退出码 $code" -ForegroundColor Red }
}

$hex = Join-Path $proj 'Objects\slave.hex'
if (Test-Path $hex) {
    $fi = Get-Item $hex
    Write-Host ("[HEX ] {0}  ({1} bytes, {2})" -f $fi.FullName, $fi.Length, $fi.LastWriteTime)
}

exit $code
