param(
    [string]$Compiler = "g++"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$testExe = Join-Path $scriptRoot "tests\test_original.exe"
$cliExe = Join-Path $scriptRoot "tests\td_tree_cli_test.exe"

Push-Location $scriptRoot
try {
    & $Compiler -Wall -Wextra -Wpedantic -O3 -std=c++17 `
        "tests\test_original.cpp" "query_decomposition.cpp" "TDTree.cpp" "Utils.cpp" `
        -o $testExe
    if ($LASTEXITCODE -ne 0) {
        throw "Test compilation failed with exit code $LASTEXITCODE"
    }

    & $testExe
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE"
    }

    & $Compiler -Wall -Wextra -Wpedantic -O3 -std=c++17 `
        "main.cpp" "query_decomposition.cpp" "TDTree.cpp" "Utils.cpp" `
        -o $cliExe
    if ($LASTEXITCODE -ne 0) {
        throw "CLI test build failed with exit code $LASTEXITCODE"
    }

    & (Join-Path $scriptRoot "tests\test_cli.ps1") -Executable $cliExe
    if (-not $?) {
        throw "CLI tests failed"
    }
}
finally {
    Pop-Location
}
