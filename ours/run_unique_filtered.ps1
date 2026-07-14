param(
    [string]$ExecutablePath = ".\td_tree.exe",
    [string]$DatasetDir = "..\Dataset",
    [string]$QueryGraph = "..\Dataset\Query5.txt",
    [int]$K = 5,
    [uint32]$LabelSeed = 42,
    [string]$OutputDir = ".\batch_results_unique_filtered",
    [string]$Compiler = "g++",
    [switch]$SkipBuild,
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
Write-Host "Output dir : $outDir"

Push-Location $scriptRoot
try {
    foreach ($dataset in $datasets) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($dataset.Name)
        $stdoutPath = Join-Path $outDir ("{0}__stdout.txt" -f $base)
        $matchOut = Join-Path $outDir ("{0}__matching_results.txt" -f $base)
        $timingOut = Join-Path $outDir ("{0}__timing_results.txt" -f $base)
        $generatedMatch = "matching_results_{0}.txt" -f $base
        $generatedTiming = "timing_results_{0}.txt" -f $base

        Write-Host "`n=== Running: $($dataset.Name) ==="
        Write-Host "& `"$exe`" `"$($dataset.FullName)`" `"$query`" $K $LabelSeed"
        if ($DryRun) {
            continue
        }

        if (Test-Path $generatedMatch) { Remove-Item $generatedMatch -Force }
        if (Test-Path $generatedTiming) { Remove-Item $generatedTiming -Force }

        & $exe $dataset.FullName $query $K $LabelSeed 2>&1 |
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
    }
}
finally {
    Pop-Location
}

Write-Host "`nBatch run complete. Results saved to: $outDir"
