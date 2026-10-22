# Сборка и заливка прошивки на Waveshare ESP32-S3-Touch-LCD-1.85.
# Использует arduino-cli, встроенный в Arduino IDE 2.x (отдельно качать не нужно).
#
#   .\tools\build.ps1                  # собрать firmware\LVGL_Arduino
#   .\tools\build.ps1 -Upload          # собрать и залить
#   .\tools\build.ps1 -Sketch firmware\my_app -Upload -Port COM4
#
param(
    [string]$Sketch = "$PSScriptRoot\..\firmware\LVGL_Arduino",
    [switch]$Upload,
    [string]$Port = "COM4"
)

$ErrorActionPreference = "Stop"

$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (-not (Test-Path $cli)) { throw "arduino-cli не найден: $cli (установлена ли Arduino IDE 2.x?)" }

# 16 MB flash, 8 MB OPI PSRAM, USB-Serial/JTAG нативный порт.
# PartitionScheme=custom -> берётся partitions.csv из папки скетча.
$fqbn = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CPUFreq=240,FlashMode=qio"

$name    = Split-Path $Sketch -Leaf
$build   = Join-Path $PSScriptRoot "..\.build\$name"

# PartitionScheme=custom берёт partitions.csv из папки скетча.
# Если его там нет — подкладываем общую таблицу из firmware\partitions-16mb.csv.
$partSrc = Join-Path $Sketch "partitions.csv"
if (-not (Test-Path $partSrc)) {
    Copy-Item (Join-Path $PSScriptRoot "..\firmware\partitions-16mb.csv") $partSrc
    Write-Host "== partitions.csv -> $name" -ForegroundColor DarkGray
}

# LVGL компилируется с <lib>\src\lv_conf.h, который перекрывает lv_conf.h скетча.
# Держим оба в синхроне с firmware\lv_conf.h.
$lvConf = Join-Path $PSScriptRoot "..\firmware\lv_conf.h"
$lvLib  = Join-Path $env:USERPROFILE "Documents\Arduino\libraries\lvgl"
$targets = @((Join-Path $Sketch "lv_conf.h"), (Join-Path $lvLib "src\lv_conf.h"))
foreach ($t in $targets) {
    $needCopy = $true
    if (Test-Path $t) {
        $needCopy = (Get-FileHash $t).Hash -ne (Get-FileHash $lvConf).Hash
    }
    if ($needCopy) {
        if ((Split-Path $t -Parent) -eq $lvLib -or (Test-Path (Split-Path $t -Parent))) {
            Copy-Item $lvConf $t -Force
            Write-Host "== lv_conf.h -> $t" -ForegroundColor DarkGray
        }
    }
}


Write-Host "== compile $name" -ForegroundColor Cyan
& $cli compile -b $fqbn --build-path $build $Sketch
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

if ($Upload) {
    Write-Host "== upload -> $Port" -ForegroundColor Cyan
    & $cli upload -b $fqbn -p $Port --input-dir $build
    if ($LASTEXITCODE -ne 0) { throw "upload failed" }
    Write-Host "== done" -ForegroundColor Green
}
