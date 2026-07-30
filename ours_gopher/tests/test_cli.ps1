param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$exe = (Resolve-Path $Executable).Path
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("ours_gopher_cli_{0}" -f [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

function Invoke-Success([string[]]$Arguments, [string]$Label) {
    & $exe @Arguments *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Failure([string[]]$Arguments, [string]$Label) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $exe @Arguments *> $null
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -eq 0) {
        throw "$Label unexpectedly succeeded"
    }
}

try {
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $data = Join-Path $tempRoot "data.txt"
    $query = Join-Path $tempRoot "query.txt"
    [System.IO.File]::WriteAllText($data, "10 20 1`n10 20 2`n", $utf8NoBom)
    [System.IO.File]::WriteAllText($query, "A B`n", $utf8NoBom)
    $required = @($data, $query, "2")

    Push-Location $tempRoot
    try {
        Invoke-Success $required "default Gopher positional CLI"
        $fullResult = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "matching_results_data.txt"))
        if ($fullResult -notmatch "mode: full") {
            throw "full result file does not record its mode"
        }

        Invoke-Success ($required + @("--count-only")) "count-only without seed"
        $countResult = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "matching_results_data.txt"))
        $countTiming = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "timing_results_data.txt"))
        if ($countResult -notmatch "mode: count-only" -or
            $countResult -match "(?m)^Match ") {
            throw "count-only result mode or row suppression is incorrect"
        }
        if ($countTiming -notmatch "(?m)^mode: count-only\r?$") {
            throw "timing file does not record count-only mode"
        }

        Invoke-Success ($required + @("42", "--count-only")) `
            "seed before count-only"
        Invoke-Success ($required + @("--count-only", "42")) `
            "count-only before seed"
        Invoke-Success ($required + @(
            "--count-only", "--engine", "gopher",
            "--selectivity", "intended",
            "--order", "reuse", "--memo", "dag",
            "--cache-mb", "1", "--cache-entry-mb", "1",
            "--cache-min-work", "0", "--no-cost-aware",
            "--threads", "4", "--parallel", "prefix",
            "--prefix-depth", "0", "--tasks-per-thread", "2",
            "--early-prune", "off",
            "--result-digest", "on", "--statistics", "off",
            "--verify-dag")) "all Gopher options"
        $optionMetrics = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "metrics_results_data.json"))
        if ($optionMetrics -notmatch '"selectivity_mode": "intended"' -or
            $optionMetrics -notmatch '"parallel_mode_requested": "prefix"' -or
            $optionMetrics -notmatch '"worker_dfs_states_total":') {
            throw "metrics do not record selectivity/parallel metadata"
        }
        Invoke-Success ($required + @(
            "--count-only", "--selectivity", "paper",
            "--threads", "1")) "single-thread paper mode"
        $singleTiming = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "timing_results_data.txt"))
        if ($singleTiming -notmatch "(?m)^selectivity_mode: paper\r?$" -or
            $singleTiming -notmatch "(?m)^threads_requested: 1\r?$" -or
            $singleTiming -notmatch "(?m)^threads_used: 1\r?$") {
            throw "single-thread selectivity/thread metadata is incorrect"
        }
        Invoke-Success ($required + @(
            "--count-only", "--engine", "legacy")) "legacy engine"

        Invoke-Failure ($required + @("--count-only", "--count-only")) `
            "duplicate count-only option"
        Invoke-Failure ($required + @("--unknown")) "unknown option"
        Invoke-Failure ($required + @("--count-only=true")) `
            "malformed count-only option"
        Invoke-Failure ($required + @("--memo", "invalid")) "invalid memo"
        Invoke-Failure ($required + @("--selectivity", "invalid")) `
            "invalid selectivity"
        Invoke-Failure ($required + @("--selectivity")) `
            "missing selectivity"
        Invoke-Failure ($required + @(
            "--selectivity", "paper", "--selectivity", "intended")) `
            "duplicate selectivity"
        Invoke-Failure ($required + @("--threads", "0")) "zero threads"
        Invoke-Failure ($required + @("--parallel", "invalid")) `
            "invalid parallel mode"
        Invoke-Failure ($required + @(
            "--parallel", "roots", "--parallel", "prefix")) `
            "duplicate parallel mode"
        Invoke-Failure ($required + @("--prefix-depth", "-1")) `
            "invalid prefix depth"
        Invoke-Failure ($required + @("--tasks-per-thread", "0")) `
            "zero tasks per thread"
        Invoke-Failure ($required + @("--early-prune", "maybe")) `
            "invalid early-prune mode"
        Invoke-Failure ($required + @("--result-digest", "maybe")) `
            "invalid digest mode"
        Invoke-Failure ($required + @("--statistics", "maybe")) `
            "invalid statistics mode"
        Invoke-Failure ($required + @("42", "43")) "duplicate positional seed"
        Invoke-Failure (@($data, $query, "2junk")) "non-integer suffix in k"
        Invoke-Failure (@($data, $query, "1")) "k below prefilter contract"
    }
    finally {
        Pop-Location
    }
}
finally {
    if (Test-Path $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}

Write-Host "Ours_gopher CLI tests passed."
