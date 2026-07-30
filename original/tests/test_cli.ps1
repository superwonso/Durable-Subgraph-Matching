param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$exe = (Resolve-Path $Executable).Path
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("original_cli_{0}" -f [System.Guid]::NewGuid().ToString("N"))
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
        Invoke-Success $required "legacy positional CLI"
        $fullResult = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "matching_results_data.txt"))
        if ($fullResult -notmatch "mode: full") {
            throw "full result file does not record its mode"
        }
        if ($fullResult -notmatch "duration_mode: cardinality") {
            throw "paper cardinality mode is not the CLI default"
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
        Invoke-Success ($required + @("--duration-mode", "consecutive")) `
            "explicit consecutive duration mode"
        $consecutiveResult = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "matching_results_data.txt"))
        $consecutiveTiming = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "timing_results_data.txt"))
        if ($consecutiveResult -notmatch "duration_mode: consecutive" -or
            $consecutiveTiming -notmatch "(?m)^duration_mode: consecutive\r?$") {
            throw "consecutive duration mode is not recorded in outputs"
        }
        Invoke-Success ($required + @("--duration-mode=cardinality", "42")) `
            "equals-form cardinality duration mode"
        Invoke-Success ($required + @(
            "--count-only", "--threads", "32")) "count-only threads"
        $threadTiming = [System.IO.File]::ReadAllText(
            (Join-Path $tempRoot "timing_results_data.txt"))
        if ($threadTiming -notmatch "(?m)^threads_requested: 32\r?$" -or
            $threadTiming -notmatch "(?m)^threads_used: 32\r?$") {
            throw "thread request/effective count is not recorded"
        }
        Invoke-Success ($required + @(
            "--count-only", "--threads=2")) "equals-form threads"

        Invoke-Failure ($required + @("--count-only", "--count-only")) `
            "duplicate count-only option"
        Invoke-Failure ($required + @("--unknown")) "unknown option"
        Invoke-Failure ($required + @("--count-only=true")) `
            "malformed count-only option"
        Invoke-Failure ($required + @("42", "43")) "duplicate positional seed"
        Invoke-Failure ($required + @("--duration-mode", "invalid")) `
            "invalid duration mode"
        Invoke-Failure ($required + @("--duration-mode")) `
            "missing duration mode"
        Invoke-Failure ($required + @(
            "--duration-mode", "cardinality",
            "--duration-mode=consecutive")) "duplicate duration mode"
        Invoke-Failure ($required + @("--threads", "0")) "zero threads"
        Invoke-Failure ($required + @("--threads", "-1")) "negative threads"
        Invoke-Failure ($required + @("--threads", "many")) `
            "non-integer threads"
        Invoke-Failure ($required + @("--threads")) "missing threads"
        Invoke-Failure ($required + @(
            "--threads", "2", "--threads=3")) "duplicate threads"
        Invoke-Failure ($required + @("--threads", "1025")) `
            "excessive threads"
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

Write-Host "Original CLI tests passed."
