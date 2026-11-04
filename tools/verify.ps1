# ============================================================
#  一条命令完成「编译 -> 烧录 -> 自检 -> 保存日志」
# ------------------------------------------------------------
#  用法:
#    powershell -ExecutionPolicy Bypass -File tools\verify.ps1
#    powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -Port COM5
#    powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -SelfTest
#
#  参数:
#    -Port      串口, 不填自动探测 (ESP32 的 303A / CH340 / CP210x)
#    -SelfTest  烧录自检固件并抓 45 秒串口日志 (不开 WiFi 也能验证接口)
#    -Env       强制指定 PlatformIO 环境 (esp32-s3-devkitc-1 / esp32-c3-devkitm-1)
# ============================================================
param(
    [string]$Port = '',
    [string]$Env = '',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $root 'firmware'
$logDir = Join-Path $root 'logs'

function Find-Port {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match '\((COM\d+)\)' -and $_.DeviceID -match 'VID_303A|VID_1A86|VID_10C4' } |
        Select-Object -First 1
    if ($dev) {
        $null = $dev.Name -match '\((COM\d+)\)'
        Write-Host "==> 自动检测到串口 $($Matches[1])  ($($dev.Name))" -ForegroundColor Cyan
        return $Matches[1]
    }
    throw '没找到 ESP32 串口。请用 -Port COMx 指定，或检查 USB 线/驱动。'
}

function Get-Chip {
    param([string]$p)
    $esp = Join-Path $env:USERPROFILE '.platformio\packages\tool-esptoolpy\esptool.py'
    $out = & python $esp --port $p --before default_reset --after hard_reset chip_id 2>&1 | Out-String
    if ($out -match 'Chip is (ESP32-[A-Za-z0-9]+)') { return $Matches[1] }
    return 'unknown'
}

if (-not $Port) { $Port = Find-Port }

$chip = Get-Chip $Port
Write-Host "==> 芯片: $chip  串口: $Port" -ForegroundColor Cyan

if (-not $Env) {
    switch -Regex ($chip) {
        'ESP32-S3' { $Env = 'esp32-s3-devkitc-1' }
        'ESP32-C3' { $Env = 'esp32-c3-devkitm-1' }
        'ESP32-S2' { $Env = 'esp32-s2-saola-1' }
        default    { $Env = 'esp32-s3-devkitc-1' }
    }
}

$pio = 'pio'
Push-Location $fw
try {
    if ($SelfTest) {
        $stEnv = if ($Env -like '*c3*') { 'c3-selftest' } else { 's3-selftest' }
        Write-Host "==> 编译自检固件 ($stEnv)" -ForegroundColor Cyan
        & $pio run -c platformio.local.ini -e $stEnv
        if ($LASTEXITCODE -ne 0) { throw '编译失败' }
        Write-Host "==> 烧录到 $Port" -ForegroundColor Cyan
        & $pio run -c platformio.local.ini -e $stEnv -t upload --upload-port $Port
        if ($LASTEXITCODE -ne 0) { throw '烧录失败' }

        $null = New-Item -ItemType Directory -Force -Path $logDir
        $logFile = Join-Path $logDir ("selftest-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Write-Host "==> 抓取串口日志 45 秒 -> $logFile" -ForegroundColor Cyan

        Start-Sleep -Seconds 2
        $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
        $sp.ReadTimeout = 500
        $sb = New-Object System.Text.StringBuilder
        try {
            $sp.Open()
            $end = (Get-Date).AddSeconds(45)
            while ((Get-Date) -lt $end) {
                $n = $sp.BytesToRead
                if ($n -gt 0) {
                    $buf = New-Object byte[] $n
                    $r = $sp.Read($buf, 0, $n)
                    [void]$sb.Append([System.Text.Encoding]::UTF8.GetString($buf, 0, $r))
                }
                Start-Sleep -Milliseconds 80
            }
        }
        finally {
            if ($sp.IsOpen) { $sp.Close() }
            $sp.Dispose()
        }
        $text = $sb.ToString()
        $text | Set-Content -Path $logFile -Encoding UTF8
        Write-Host '=============== 自检日志 ===============' -ForegroundColor Green
        Write-Host $text
        Write-Host '========================================' -ForegroundColor Green
        Write-Host "日志已保存: $logFile" -ForegroundColor Cyan
    }
    else {
        Write-Host "==> 编译 ($Env)" -ForegroundColor Cyan
        & $pio run -e $Env
        if ($LASTEXITCODE -ne 0) { throw '编译失败' }
        Write-Host "==> 烧录到 $Port" -ForegroundColor Cyan
        & $pio run -e $Env -t upload --upload-port $Port
        if ($LASTEXITCODE -ne 0) { throw '烧录失败' }
        Write-Host '==> 完成。查看日志: pio device monitor -b 115200' -ForegroundColor Green
    }
}
finally {
    Pop-Location
}

Write-Host ''
Write-Host '接线提醒: SG90 橙=GPIO4, 红=外部5V+, 棕=外部5V- 且接 ESP32 GND' -ForegroundColor Yellow
