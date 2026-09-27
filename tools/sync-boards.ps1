#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("new", "zlx", "old", "ili")]
    [string]$From,

    [switch]$ForceLogic,
    [switch]$WhatIf
)

$ErrorActionPreference = "Stop"
$ScriptPath = $MyInvocation.MyCommand.Path
if (-not [System.IO.Path]::IsPathRooted($ScriptPath)) {
    $ScriptPath = Join-Path (Get-Location) $ScriptPath
}
$Root = Split-Path -Parent (Split-Path -Parent $ScriptPath)
$OldProj = Join-Path $Root "Project-ESPNow"
$NewProj = Join-Path $Root "Project-ESPNow-ZLX"

if ($From -in @("new", "zlx")) {
    $SrcRoot = $NewProj
    $DstRoot = $OldProj
    $SrcName = "ZLX-new"
    $DstName = "OLD"
} else {
    $SrcRoot = $OldProj
    $DstRoot = $NewProj
    $SrcName = "OLD"
    $DstName = "ZLX-new"
}

$SafeFiles = @(
    "src\chat_ui_impl.inc",
    "src\cn_font_data.h",
    "src\cn_pinyin_data.h",
    "src\cn_text.cpp",
    "src\cn_text.h",
    "src\drawing_history.h",
    "src\esp_now_handler.cpp",
    "src\esp_now_handler.h",
    "src\game_arcade.h",
    "src\game_arcade_more.cpp",
    "src\game_arcade_more.h",
    "src\power_manager.cpp",
    "src\power_manager.h",
    "src\soup_puzzles.h",
    "src\touch_handler.h",
    "src\transport_manager.cpp",
    "src\transport_manager.h",
    "src\ui_manager.h",
    "partitions.csv"
)

$LogicFiles = @(
    "src\game_arcade.cpp",
    "src\touch_handler.cpp",
    "src\ui_manager.cpp"
)

$NeverSync = @(
    "User_Setup.h",
    "src\config.h",
    "Project-ESPNow.ino",
    "Project-ESPNow-ZLX.ino",
    "README-burn.md"
)

function Copy-One($rel) {
    $s = Join-Path $SrcRoot $rel
    $d = Join-Path $DstRoot $rel
    if (-not (Test-Path $s)) {
        Write-Host ("  SKIP missing: {0}" -f $rel) -ForegroundColor DarkYellow
        return
    }
    $dir = Split-Path -Parent $d
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $same = $false
    if (Test-Path $d) {
        $same = ((Get-FileHash $s -Algorithm MD5).Hash -eq (Get-FileHash $d -Algorithm MD5).Hash)
    }
    if ($same) {
        Write-Host ("  SAME {0}" -f $rel)
        return
    }
    if ($WhatIf) {
        Write-Host ("  WOULD COPY {0}" -f $rel) -ForegroundColor Cyan
        return
    }
    Copy-Item $s $d -Force
    Write-Host ("  COPY  {0}" -f $rel) -ForegroundColor Green
}

Write-Host ""
Write-Host ("Sync: {0}  ->  {1}" -f $SrcName, $DstName) -ForegroundColor Cyan
Write-Host "---- safe shared ----"
foreach ($f in $SafeFiles) { Copy-One $f }

Write-Host "---- logic half-shared ----"
foreach ($f in $LogicFiles) {
    $s = Join-Path $SrcRoot $f
    $d = Join-Path $DstRoot $f
    if (-not (Test-Path $s)) { continue }
    $diff = $true
    if (Test-Path $d) {
        $diff = ((Get-FileHash $s -Algorithm MD5).Hash -ne (Get-FileHash $d -Algorithm MD5).Hash)
    }
    if (-not $diff) {
        Write-Host ("  SAME {0}" -f $f)
        continue
    }
    if ($ForceLogic) {
        Copy-One $f
        Write-Host ("  WARN overwritten {0} - review board-specific diffs" -f $f) -ForegroundColor Yellow
    } else {
        Write-Host ("  DIFF {0}  (use -ForceLogic to overwrite)" -f $f) -ForegroundColor Magenta
    }
}

Write-Host "---- never auto-sync ----"
foreach ($f in $NeverSync) {
    Write-Host ("  KEEP {0}" -f $f) -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "Done. Feature changes must update BOTH boards." -ForegroundColor Cyan
Write-Host ""
