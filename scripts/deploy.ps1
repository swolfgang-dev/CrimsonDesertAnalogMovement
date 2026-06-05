param(
    [string]$GameBin = 'D:\Games\Steam\steamapps\common\Crimson Desert\bin64',
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$AsiPath = Join-Path $ProjectRoot "build\$Configuration\CrimsonDesertAnalogMovement.asi"
$IniPath = Join-Path $ProjectRoot 'CrimsonDesertAnalogMovement.ini'

if (-not (Test-Path -LiteralPath $GameBin)) {
    throw "Game bin folder not found: $GameBin"
}

if (-not (Test-Path -LiteralPath $AsiPath)) {
    throw "Built ASI not found: $AsiPath"
}

Copy-Item -LiteralPath $AsiPath -Destination (Join-Path $GameBin 'CrimsonDesertAnalogMovement.asi') -Force
Copy-Item -LiteralPath $IniPath -Destination (Join-Path $GameBin 'CrimsonDesertAnalogMovement.ini') -Force

Write-Host "Deployed CrimsonDesertAnalogMovement to $GameBin"
