Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$output = Join-Path $scriptRoot "td_tree_bfs.exe"

Push-Location $scriptRoot
try {
    g++ -Wall -Wextra -O2 -std=c++17 `
        main.cpp `
        query_decomposition.cpp `
        TDTree.cpp `
        Utils.cpp `
        GpuCandidateExpander.cpp `
        -o $output
}
finally {
    Pop-Location
}

Write-Host "Built CPU fallback executable: $output"
