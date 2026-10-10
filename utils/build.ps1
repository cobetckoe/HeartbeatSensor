<#
.SYNOPSIS
    构建固件 —— 坎川工业 ESP32-C3 毫米波雷达跟踪器。

.DESCRIPTION
    封装 pio 命令，不用每次手打长路径。

    ★ 脚本为什么会「挑」pio：
      本机存在多个 pio，其中 C:\.platformio\penv\Scripts\pio.exe 缺 intelhex，
      在链接 elf→hex 时会报 ModuleNotFoundError: No module named 'intelhex'。
      所以这里优先选不在 .platformio\penv 下的那个。

.PARAMETER Clean
    先 fullclean（清掉 .pio\build 全部中间产物）再构建。
    只改了头文件却一直不重新编译时用它。

.PARAMETER Monitor
    构建成功后直接打开串口监视器（115200）。

.PARAMETER Pio
    手动指定 pio 可执行文件路径。

.EXAMPLE
    .\utils\build.ps1
    普通增量构建。

.EXAMPLE
    .\utils\build.ps1 -Clean -Monitor
    干净重建，然后进串口看输出。

.NOTES
    历史踩坑：曾出现 .sconsign311.tmp 的 FileNotFoundError，
    删除 arduino\ESP32_C3_Radar_Tracker\.pio\build\esp32c3\.sconsign311* 后重建即可。
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Monitor,
    [string]$Pio
)

$ErrorActionPreference = 'Stop'

# ── 工程根目录（本脚本在 utils\ 下，上一层就是根）──
$Root = Split-Path -Parent $PSScriptRoot
$Sketch = Join-Path $Root 'arduino\ESP32_C3_Radar_Tracker'

if (-not (Test-Path (Join-Path $Sketch 'ESP32_C3_Radar_Tracker.ino'))) {
    throw "找不到固件源码：$Sketch\ESP32_C3_Radar_Tracker.ino"
}

# ── 挑一个能用的 pio ──
function Resolve-Pio {
    param([string]$Explicit)

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Explicit) { $candidates.Add($Explicit) }

    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add($cmd.Source) }

    $candidates.Add((Join-Path $env:ProgramFiles 'Python311\Scripts\pio.exe'))

    # 第一轮跳过 penv（缺 intelhex），都没命中再考虑它
    foreach ($pass in 1, 2) {
        foreach ($c in $candidates) {
            if (-not $c -or -not (Test-Path $c)) { continue }
            if ($pass -eq 1 -and $c -like '*.platformio\penv\*') { continue }
            return $c
        }
    }
    throw '找不到可用的 pio。请先安装 PlatformIO Core，或用 -Pio 指定路径。'
}

$pio = Resolve-Pio -Explicit $Pio
Write-Host "使用 pio : $pio" -ForegroundColor DarkGray
Write-Host "工程目录 : $Sketch" -ForegroundColor DarkGray
Write-Host ''

try {
    if ($Clean) {
        Write-Host '── fullclean ──' -ForegroundColor Cyan
        & $pio run -d $Sketch -t fullclean
        if ($LASTEXITCODE -ne 0) { throw "fullclean 失败（exit $LASTEXITCODE）" }
    }

    Write-Host '── build ──' -ForegroundColor Cyan
    & $pio run -d $Sketch
    if ($LASTEXITCODE -ne 0) { throw "构建失败（exit $LASTEXITCODE）" }

    Write-Host ''
    Write-Host '[OK] 构建完成' -ForegroundColor Green
    Write-Host '     下一步：.\utils\flash.ps1   烧录到板子' -ForegroundColor Green

    if ($Monitor) {
        Write-Host ''
        Write-Host '── monitor（Ctrl+C 退出）──' -ForegroundColor Cyan
        & $pio device monitor -d $Sketch -b 115200
    }
}
finally {
    # 没有需要清理的临时状态，这里只为保证异常时提示不被吞掉
}
