# 一键编译/烧录/看日志 (Windows PowerShell)
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\pio.ps1 -Task build
#   powershell -ExecutionPolicy Bypass -File tools\pio.ps1 -Task upload -Port COM5
#   powershell -ExecutionPolicy Bypass -File tools\pio.ps1 -Task monitor -Port COM5
param(
    [ValidateSet('setup', 'build', 'upload', 'monitor', 'all', 'clean')]
    [string]$Task = 'build',
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $root 'firmware'
$venv = Join-Path $root '.venv'
$pio = Join-Path $venv 'Scripts\pio.exe'

function Find-Python {
    foreach ($c in @('python', 'py', 'python3')) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw '未找到 Python，请先安装 Python 3.9+ 并勾选 Add to PATH'
}

function Ensure-PlatformIO {
    if (Test-Path $pio) { return }
    Write-Host '==> 首次运行，创建虚拟环境并安装 PlatformIO（需要联网）' -ForegroundColor Cyan
    $py = Find-Python
    & $py -m venv $venv
    & (Join-Path $venv 'Scripts\python.exe') -m pip install --upgrade pip
    & (Join-Path $venv 'Scripts\python.exe') -m pip install -U platformio
}

Ensure-PlatformIO

Push-Location $fw
try {
    switch ($Task) {
        'setup' { & $pio pkg install }
        'clean' { & $pio run -t clean }
        'build' { & $pio run }
        'upload' {
            if ($Port) { & $pio run -t upload --upload-port $Port }
            else { & $pio run -t upload }
        }
        'monitor' {
            if ($Port) { & $pio device monitor -p $Port -b 115200 }
            else { & $pio device monitor -b 115200 }
        }
        'all' {
            if ($Port) { & $pio run -t upload --upload-port $Port }
            else { & $pio run -t upload }
            if ($LASTEXITCODE -eq 0) {
                if ($Port) { & $pio device monitor -p $Port -b 115200 }
                else { & $pio device monitor -b 115200 }
            }
        }
    }
}
finally {
    Pop-Location
}
