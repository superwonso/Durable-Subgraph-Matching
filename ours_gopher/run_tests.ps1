param(
    [string]$Compiler = "g++",
    [switch]$StrictWarnings
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("ours_gopher_build_{0}" -f [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $buildRoot | Out-Null
$oursTestExe = Join-Path $buildRoot "test_ours.exe"
$gopherTestExe = Join-Path $buildRoot "test_gopher.exe"
$parallelTestExe = Join-Path $buildRoot "test_parallel_prefix.exe"
$cliExe = Join-Path $buildRoot "td_tree_gopher_cli_test.exe"

Push-Location $scriptRoot
try {
    $warningFlags = @("-Wall", "-Wextra", "-Wpedantic")
    if ($StrictWarnings) {
        $warningFlags += @("-Wconversion", "-Wshadow", "-Werror")
    }
    & $Compiler @warningFlags -O3 -std=c++17 `
        "tests\test_ours.cpp" "query_decomposition.cpp" `
        "TDTree.cpp" "Utils.cpp" -pthread -o $oursTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Ours regression test compilation failed with exit code $LASTEXITCODE"
    }
    & $oursTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Ours regression tests failed with exit code $LASTEXITCODE"
    }

    $gopherSources = @(
        "CandidateDag.cpp",
        "DagExecutor.cpp",
        "GopherMatcher.cpp",
        "SearchStatistics.cpp",
        "query_decomposition.cpp",
        "TDTree.cpp",
        "Utils.cpp"
    )
    & $Compiler @warningFlags -O3 -std=c++17 `
        "tests\test_gopher.cpp" @gopherSources `
        -pthread -o $gopherTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Gopher test compilation failed with exit code $LASTEXITCODE"
    }
    & $gopherTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Gopher tests failed with exit code $LASTEXITCODE"
    }

    & $Compiler @warningFlags -O3 -std=c++17 `
        "tests\test_parallel_prefix.cpp" @gopherSources `
        -pthread -o $parallelTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Parallel prefix test compilation failed with exit code $LASTEXITCODE"
    }
    & $parallelTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "Parallel prefix tests failed with exit code $LASTEXITCODE"
    }

    & $Compiler @warningFlags -O3 -std=c++17 `
        "main.cpp" @gopherSources -pthread -o $cliExe
    if ($LASTEXITCODE -ne 0) {
        throw "CLI test build failed with exit code $LASTEXITCODE"
    }
    & (Join-Path $scriptRoot "tests\test_cli.ps1") `
        -Executable $cliExe
    if (-not $?) {
        throw "CLI tests failed"
    }
}
finally {
    Pop-Location
    if (Test-Path $buildRoot) {
        Remove-Item -LiteralPath $buildRoot -Recurse -Force
    }
}

Write-Host "All ours_gopher test suites passed."
