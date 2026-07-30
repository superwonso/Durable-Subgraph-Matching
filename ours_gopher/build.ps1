param(
    [string]$Compiler = "g++",
    [string]$OutputPath = ".\td_tree_gopher.exe",
    [switch]$StrictWarnings
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$output = Join-Path $scriptRoot $OutputPath

Push-Location $scriptRoot
try {
    $warningFlags = @("-Wall", "-Wextra", "-Wpedantic")
    if ($StrictWarnings) {
        $warningFlags += @("-Wconversion", "-Wshadow", "-Werror")
    }
    & $Compiler @warningFlags -O3 -std=c++17 `
        "main.cpp" "CandidateDag.cpp" "DagExecutor.cpp" `
        "GopherMatcher.cpp" "SearchStatistics.cpp" `
        "query_decomposition.cpp" "TDTree.cpp" "Utils.cpp" `
        -pthread -o $output
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
    Write-Host "Built: $output"
}
finally {
    Pop-Location
}
