param(
    [string]$Compiler = "g++"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$testExe = Join-Path $scriptRoot "tests\test_ours.exe"

Push-Location $scriptRoot
try {
    & $Compiler -Wall -Wextra -Wpedantic -O3 -std=c++17 `
        "tests\test_ours.cpp" "query_decomposition.cpp" "TDTree.cpp" "Utils.cpp" `
        -o $testExe
    if ($LASTEXITCODE -ne 0) {
        throw "Test compilation failed with exit code $LASTEXITCODE"
    }

    & $testExe
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
