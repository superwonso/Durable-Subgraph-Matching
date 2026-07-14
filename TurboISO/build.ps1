[CmdletBinding()]
param(
    [string]$Compiler = "g++"
)

$ErrorActionPreference = "Stop"
$sourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Push-Location $sourceDir
try {
    & $Compiler -std=c++17 -O3 -Wall -Wextra -Wpedantic `
        .\main.cpp .\Graph.cpp -o .\turbo.exe
    if ($LASTEXITCODE -ne 0) {
        throw "TurboISO build failed with exit code $LASTEXITCODE"
    }

    & $Compiler -std=c++17 -O3 -Wall -Wextra -Wpedantic `
        .\convert_temporal.cpp -o .\turbo_convert.exe
    if ($LASTEXITCODE -ne 0) {
        throw "TurboISO converter build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host "Built TurboISO/turbo.exe and TurboISO/turbo_convert.exe"
