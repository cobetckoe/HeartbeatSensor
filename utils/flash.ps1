<#
.SYNOPSIS
    构建并烧录固件 —— 坎川工业 心跳传感器（ESP32-C3）。

.DESCRIPTION
    默认走 PlatformIO 标准上传流程（pio run -t upload），会自动编译。

    加 -Raw 则改用 esptool 手动写 4 个分区（bootloader / partitions /
    boot_app0 / firmware）。这条路径在本机实测过（COM30 @460800，
    4 段 Hash 全部校验通过），当 pio upload 抽风时用它兜底。

.PARAMETER Port
    串口名，例如 COM30。不填则自动探测；探测到多个会让你选。

.PARAMETER Baud
    烧录波特率，默认 460800（仅 -Raw 时生效）。

.PARAMETER Raw
    绕过 pio，直接用 esptool 手动写分区。

.PARAMETER Monitor
    烧录完成后打开串口监视器。

.PARAMETER SkipBuild
    -Raw 模式下跳过前置编译（假定固件已构建好）。

.EXAMPLE
    .\utils\flash.ps1
    自动找串口，编译 + 烧录。

.EXAMPLE
    .\utils\flash.ps1 -Port COM30 -Raw -Monitor
    指定串口，用 esptool 手动烧录，然后看输出。

.NOTES
    烧不进去时先按住 BOOT 再点一下 RESET 让板子进下载模式。
#>
[CmdletBinding()]
param(
    [string]$Port,
    [int]$Baud = 460800,
    [switch]$Raw,
    [switch]$Monitor,
    [switch]$SkipBuild,
    [string]$Pio
)

$ErrorActionPreference = 'Stop'

$Root   = Split-Path -Parent $PSScriptRoot
$Sketch = Join-Path $Root 'src\master'

# ── 找 pio（同 build.ps1：优先非 penv）──
function Resolve-Pio {
    param([string]$Explicit)

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Explicit) { $candidates.Add($Explicit) }

    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add($cmd.Source) }

    $candidates.Add((Join-Path $env:ProgramFiles 'Python311\Scripts\pio.exe'))

    foreach ($pass in 1, 2) {
        foreach ($c in $candidates) {
            if (-not $c -or -not (Test-Path $c)) { continue }
            if ($pass -eq 1 -and $c -like '*.platformio\penv\*') { continue }
            return $c
        }
    }
    throw '找不到可用的 pio。'
}

# ── 找 python（给 esptool 用）──
function Resolve-Python {
    $candidates = @(
        (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'),
        (Join-Path $env:ProgramFiles 'Python311\python.exe')
    )
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw '找不到 python，-Raw 模式需要它来跑 esptool.py。'
}

# ── 探测串口 ──
function Resolve-Port {
    param([string]$Explicit)

    if ($Explicit) { return $Explicit }

    $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($ports.Count -eq 0) {
        throw '没有探测到任何串口。请检查 USB 线（必须是数据线）和驱动。'
    }
    if ($ports.Count -eq 1) {
        Write-Host "自动选择串口：$($ports[0])" -ForegroundColor DarkGray
        return $ports[0]
    }

    Write-Host '检测到多个串口：' -ForegroundColor Yellow
    for ($i = 0; $i -lt $ports.Count; $i++) {
        Write-Host ("  [{0}] {1}" -f ($i + 1), $ports[$i])
    }
    $sel = Read-Host '请选择序号（默认 1）'
    if ([string]::IsNullOrWhiteSpace($sel)) { $sel = '1' }
    $idx = [int]$sel - 1
    if ($idx -lt 0 -or $idx -ge $ports.Count) { throw "无效的序号：$sel" }
    return $ports[$idx]
}

$port = Resolve-Port -Explicit $Port
Write-Host "串口     : $port" -ForegroundColor DarkGray

$pio = $null
if (-not $Raw) {
    $pio = Resolve-Pio -Explicit $Pio
    Write-Host "使用 pio : $pio" -ForegroundColor DarkGray
}
Write-Host ''

if (-not $Raw) {
    # ── 标准路径：pio 自己编译 + 上传 ──
    Write-Host '── build + upload ──' -ForegroundColor Cyan
    & $pio run -d $Sketch -t upload --upload-port $port
    if ($LASTEXITCODE -ne 0) {
        throw "上传失败（exit $LASTEXITCODE）。可试试：.\utils\flash.ps1 -Port $port -Raw"
    }
}
else {
    # ── 兜底路径：esptool 手动写 4 个分区（本机实测可用）──
    $python = Resolve-Python

    $pkgRoot   = Join-Path $env:USERPROFILE '.platformio\packages'
    $esptool   = Join-Path $pkgRoot 'tool-esptoolpy\esptool.py'
    $bootApp0  = Join-Path $pkgRoot 'framework-arduinoespressif32\tools\partitions\boot_app0.bin'
    $buildDir  = Join-Path $Sketch '.pio\build\esp32c3'

    foreach ($f in @($esptool, $bootApp0)) {
        if (-not (Test-Path $f)) {
            throw "缺少 $f。先跑一次 .\utils\build.ps1 让 PlatformIO 把包装好。"
        }
    }

    if (-not $SkipBuild) {
        if (-not $pio) { $pio = Resolve-Pio -Explicit $Pio }
        Write-Host '── build ──' -ForegroundColor Cyan
        & $pio run -d $Sketch
        if ($LASTEXITCODE -ne 0) { throw "构建失败（exit $LASTEXITCODE）" }
    }

    $images = @{
        'bootloader.bin' = [Int64]0x0
        'partitions.bin' = [Int64]0x8000
        'firmware.bin'   = [Int64]0x10000
    }
    foreach ($name in $images.Keys) {
        $p = Join-Path $buildDir $name
        if (-not (Test-Path $p)) {
            throw "找不到固件镜像 $p。先跑 .\utils\build.ps1。"
        }
    }

    Write-Host ''
    Write-Host "── esptool 烧录 (chip=esp32c3, $port @ $Baud) ──" -ForegroundColor Cyan
    $args = @(
        $esptool,
        '--chip', 'esp32c3',
        '--port', $port,
        '--baud', "$Baud",
        'write_flash', '-z',
        '--flash_mode', 'dio',
        '--flash_freq', '80m',
        '--flash_size', '4MB',
        '0x0',    (Join-Path $buildDir 'bootloader.bin'),
        '0x8000', (Join-Path $buildDir 'partitions.bin'),
        '0xe000', $bootApp0,
        '0x10000', (Join-Path $buildDir 'firmware.bin')
    )
    & $python @args
    if ($LASTEXITCODE -ne 0) {
        throw "esptool 烧录失败（exit $LASTEXITCODE）。试试先按住 BOOT 再点 RESET。"
    }
}

Write-Host ''
Write-Host "[OK] 烧录完成（$port）" -ForegroundColor Green

if ($Monitor) {
    if (-not $pio) { $pio = Resolve-Pio -Explicit $Pio }
    Write-Host ''
    Write-Host '── monitor（Ctrl+C 退出）──' -ForegroundColor Cyan
    # 必须显式传 -p $port：否则 pio device monitor 会自动挑一个串口，
    # 在多设备机器上很容易连到别的口（实测曾连到 COM3），表现为「监视器一片空白」。
    & $pio device monitor -d $Sketch -p $port -b 115200
}
