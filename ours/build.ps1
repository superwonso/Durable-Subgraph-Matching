param(
    [string]$Compiler = "g++",
    [string]$OutputPath = ".\td_tree.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$output = Join-Path $scriptRoot $OutputPath

Push-Location $scriptRoot
try {
    & $Compiler -Wall -Wextra -Wpedantic -O3 -std=c++17 `
        "main.cpp" "query_decomposition.cpp" "TDTree.cpp" "Utils.cpp" `
        -o $output
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
    Write-Host "Built: $output"
}
finally {
    Pop-Location
}
