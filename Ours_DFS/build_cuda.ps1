Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$output = Join-Path $scriptRoot "td_tree_bfs_cuda.exe"
$nvcc = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe"

if (-not (Test-Path $nvcc)) {
    throw "nvcc not found at $nvcc"
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($null -eq $cl) {
    throw "cl.exe was not found in PATH. On Windows, nvcc needs the Visual Studio C++ toolchain. Open a Developer PowerShell or install Build Tools first."
}

Push-Location $scriptRoot
try {
    & $nvcc `
        -std=c++17 `
        -O2 `
        -DBUILD_WITH_CUDA `
        main.cpp `
        query_decomposition.cpp `
        TDTree.cpp `
        Utils.cpp `
        GpuCandidateExpander.cpp `
        GpuCandidateExpander.cu `
        -o $output
}
finally {
    Pop-Location
}

Write-Host "Built CUDA executable: $output"
