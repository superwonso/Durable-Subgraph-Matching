param(
    [string]$ExecutablePath = ".\td_tree_gopher.exe",
    [string]$DatasetDir = "..\Dataset",
    [string]$QueryGraph = "..\Dataset\Query5.txt",
    [int]$K = 5,
    [uint32]$LabelSeed = 42,
    [string]$OutputDir = ".\batch_results_unique_filtered",
    [string]$Compiler = "g++",
    [ValidateSet("paper", "reuse")]
    [string]$Order = "reuse",
    [ValidateSet("none", "structural", "temporal", "dag")]
    [string]$Memo = "dag",
    [int]$CacheMB = 256,
    [int]$Threads = 1,
    [switch]$Statistics,
    [switch]$SkipBuild,
    [switch]$CountOnly,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $SkipBuild) {
    & (Join-Path $scriptRoot "build.ps1") `
        -Compiler $Compiler -OutputPath $ExecutablePath
}

$exe = (Resolve-Path (Join-Path $scriptRoot $ExecutablePath)).Path
$dataDir = (Resolve-Path (Join-Path $scriptRoot $DatasetDir)).Path
$query = (Resolve-Path (Join-Path $scriptRoot $QueryGraph)).Path
$outDir = Join-Path $scriptRoot $OutputDir
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$datasets = Get-ChildItem -Path $dataDir -File |
    Where-Object { $_.Name -like "*unique*_filtered.txt" } |
    Sort-Object Name
if ($datasets.Count -eq 0) {
    Write-Host "No datasets matched '*unique*_filtered.txt' in $dataDir"
    exit 0
}

Write-Host "Found $($datasets.Count) datasets"
Write-Host "Executable : $exe"
Write-Host "Query graph: $query"
Write-Host "k          : $K"
Write-Host "Label seed : $LabelSeed"
Write-Host "Count only : $CountOnly"
Write-Host "Order/memo : $Order / $Memo"
Write-Host "Threads    : $Threads"
Write-Host "Output dir : $outDir"

Push-Location $scriptRoot
try {
    foreach ($dataset in $datasets) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($dataset.Name)
        $stdoutPath = Join-Path $outDir ("{0}__stdout.txt" -f $base)
        $matchOut = Join-Path $outDir ("{0}__matching_results.txt" -f $base)
        $timingOut = Join-Path $outDir ("{0}__timing_results.txt" -f $base)
        $metricsOut = Join-Path $outDir ("{0}__metrics.json" -f $base)
        $generatedMatch = "matching_results_{0}.txt" -f $base
        $generatedTiming = "timing_results_{0}.txt" -f $base
        $generatedMetrics = "metrics_results_{0}.json" -f $base

        Write-Host "`n=== Running: $($dataset.Name) ==="
        $runArguments = @(
            $dataset.FullName, $query, $K, $LabelSeed,
            "--engine", "gopher",
            "--order", $Order,
            "--memo", $Memo,
            "--cache-mb", $CacheMB,
            "--threads", $Threads,
            "--result-digest", "off",
            "--statistics", $(if ($Statistics) { "on" } else { "off" })
        )
        if ($CountOnly) {
            $runArguments += "--count-only"
        }
        Write-Host "& `"$exe`" $($runArguments -join ' ')"
        if ($DryRun) {
            continue
        }

        if (Test-Path $generatedMatch) { Remove-Item $generatedMatch -Force }
        if (Test-Path $generatedTiming) { Remove-Item $generatedTiming -Force }
        if (Test-Path $generatedMetrics) { Remove-Item $generatedMetrics -Force }

        & $exe @runArguments 2>&1 |
            Tee-Object -FilePath $stdoutPath
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Execution failed for $($dataset.Name) (exit code: $LASTEXITCODE)"
            continue
        }

        if (Test-Path $generatedMatch) {
            Copy-Item $generatedMatch $matchOut -Force
        } else {
            Write-Warning "$generatedMatch was not produced"
        }
        if (Test-Path $generatedTiming) {
            Copy-Item $generatedTiming $timingOut -Force
        } else {
            Write-Warning "$generatedTiming was not produced"
        }
        if (Test-Path $generatedMetrics) {
            Copy-Item $generatedMetrics $metricsOut -Force
        } else {
            Write-Warning "$generatedMetrics was not produced"
        }
    }
}
finally {
    Pop-Location
}

Write-Host "`nBatch run complete. Results saved to: $outDir"
