#Requires -Version 5.1
param(
    [Parameter(Position = 0)]
    [ValidateSet("new", "zlx", "old", "ili", "status", "menu", "")]
    [string]$Target = "menu"
)

$ErrorActionPreference = "Stop"
$ScriptPath = $MyInvocation.MyCommand.Path
if (-not [System.IO.Path]::IsPathRooted($ScriptPath)) {
    $ScriptPath = Join-Path (Get-Location) $ScriptPath
}
$Root = Split-Path -Parent (Split-Path -Parent $ScriptPath)
if (-not (Test-Path (Join-Path $Root "Project-ESPNow"))) {
    $Root = "D:\esp32now"
}

$OldProj = Join-Path $Root "Project-ESPNow"
$NewProj = Join-Path $Root "Project-ESPNow-ZLX"
$ActiveFile = Join-Path $Root "ACTIVE_DEVICE.txt"
$LibSetup = Join-Path $env:USERPROFILE "Documents\Arduino\libraries\TFT_eSPI\User_Setup.h"
$LibDir = Split-Path -Parent $LibSetup

function Get-Active {
    if (Test-Path $ActiveFile) {
        return (Get-Content $ActiveFile -Raw).Trim()
    }
    return "unknown"
}

function Show-Status {
    $active = Get-Active
    Write-Host ""
    Write-Host "======== ESPNow device switch ========" -ForegroundColor Cyan
    Write-Host ("ACTIVE_DEVICE : {0}" -f $active)
    if (Test-Path $LibSetup) {
        $txt = Get-Content $LibSetup -Raw
        if ($txt -match '(?m)^\s*#define\s+ST7789_DRIVER') {
            Write-Host "TFT_eSPI      : ST7789 (ZLX new board)" -ForegroundColor Green
        } elseif ($txt -match '(?m)^\s*#define\s+ILI9341_2_DRIVER') {
            Write-Host "TFT_eSPI      : ILI9341_2 (old board)" -ForegroundColor Yellow
        } else {
            Write-Host "TFT_eSPI      : unknown driver in User_Setup.h" -ForegroundColor Red
        }
        Write-Host ("Lib path      : {0}" -f $LibSetup)
    } else {
        Write-Host ("Missing TFT_eSPI User_Setup.h: {0}" -f $LibSetup) -ForegroundColor Red
    }
    Write-Host ("Old sketch    : {0}" -f (Join-Path $OldProj "Project-ESPNow.ino"))
    Write-Host ("New sketch    : {0}" -f (Join-Path $NewProj "Project-ESPNow-ZLX.ino"))
    Write-Host "======================================="
    Write-Host ""
}

function Switch-To([string]$Which) {
    if (-not (Test-Path $LibDir)) {
        throw ("TFT_eSPI lib not found: {0}" -f $LibDir)
    }

    if ($Which -eq "new") {
        $src = Join-Path $NewProj "User_Setup.h"
        $label = "new"
        $title = "NEW board ZLX (ST7789)"
        $sketch = Join-Path $NewProj "Project-ESPNow-ZLX.ino"
        $color = "Green"
        $bakName = "User_Setup_ZLX_ESP32_1.h"
    } else {
        $src = Join-Path $OldProj "User_Setup.h"
        $label = "old"
        $title = "OLD board (ILI9341)"
        $sketch = Join-Path $OldProj "Project-ESPNow.ino"
        $color = "Yellow"
        $bakName = "User_Setup_OLD_ILI9341.h"
    }

    if (-not (Test-Path $src)) {
        throw ("Missing source setup: {0}" -f $src)
    }

    $stamp = Get-Date -Format "yyyyMMddHHmmss"
    if (Test-Path $LibSetup) {
        Copy-Item $LibSetup (Join-Path $LibDir ("User_Setup.h.bak-before-switch-" + $stamp)) -Force
    }

    Copy-Item $src $LibSetup -Force
    Copy-Item $src (Join-Path $LibDir $bakName) -Force
    Copy-Item $src (Join-Path $Root "User_Setup.h") -Force
    Set-Content -Path $ActiveFile -Value $label -Encoding ASCII -NoNewline

    Write-Host ""
    Write-Host ("Switched to: {0}" -f $title) -ForegroundColor $color
    Write-Host ("Wrote     : {0}" -f $LibSetup)
    Write-Host ("Open      : {0}" -f $sketch)
    Write-Host "Arduino: Partition = Huge APP (3MB No OTA), then Upload."
    Write-Host ""

    if (Test-Path $sketch) {
        explorer.exe ("/select," + $sketch)
    }
}

function Show-Menu {
    Show-Status
    Write-Host "1) Switch to NEW board ZLX (ST7789)  -> Project-ESPNow-ZLX"
    Write-Host "2) Switch to OLD board (ILI9341)     -> Project-ESPNow"
    Write-Host "3) Status only"
    Write-Host "0) Exit"
    Write-Host ""
    $c = Read-Host "Select"
    switch ($c) {
        "1" { Switch-To "new"; Show-Status }
        "2" { Switch-To "old"; Show-Status }
        "3" { Show-Status }
        default { }
    }
}

switch ($Target) {
    { $_ -in @("new", "zlx") } { Switch-To "new"; Show-Status }
    { $_ -in @("old", "ili") } { Switch-To "old"; Show-Status }
    "status" { Show-Status }
    default { Show-Menu }
}
