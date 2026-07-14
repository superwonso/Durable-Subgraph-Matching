param(
    [string]$ExecutablePath = ".\td_tree.exe",
    [string]$DatasetDir = "..\Dataset",
    [string]$DatasetPattern = "*unique*_filtered.txt",
    [string]$QueryGraph = "..\Dataset\Query5.txt",
    [int]$K = 5,
    [uint32]$LabelSeed = 42,
    [string]$OutputDir = ".\batch_results_original",
    [string]$Compiler = "g++",
    [switch]$CountOnly,
    [switch]$SkipBuild,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Get-AbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $scriptRoot $Path))
}

if (-not $SkipBuild) {
    & (Join-Path $scriptRoot "build.ps1") `
        -Compiler $Compiler -OutputPath $ExecutablePath
}

$exe = (Resolve-Path (Get-AbsolutePath $ExecutablePath)).Path
$dataDir = (Resolve-Path (Get-AbsolutePath $DatasetDir)).Path
$query = (Resolve-Path (Get-AbsolutePath $QueryGraph)).Path
$outDir = Get-AbsolutePath $OutputDir
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$datasets = @(Get-ChildItem -Path $dataDir -File |
    Where-Object { $_.Name -like $DatasetPattern } |
    Sort-Object Name)
if ($datasets.Count -eq 0) {
    Write-Host "No datasets matched '$DatasetPattern' in $dataDir"
    exit 0
}

Write-Host "Found $($datasets.Count) datasets"
Write-Host "Executable : $exe"
Write-Host "Data pattern: $DatasetPattern"
Write-Host "Query graph: $query"
Write-Host "k          : $K"
Write-Host "Label seed : $LabelSeed"
Write-Host "Output mode: $(if ($CountOnly) { 'count-only' } else { 'full' })"
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
        $runArguments = @($dataset.FullName, $query, $K, $LabelSeed)
        if ($CountOnly) { $runArguments += "--count-only" }
        $optionText = if ($CountOnly) { " --count-only" } else { "" }
        Write-Host "& `"$exe`" `"$($dataset.FullName)`" `"$query`" $K $LabelSeed$optionText"
        if ($DryRun) {
            continue
        }

        if (Test-Path $generatedMatch) { Remove-Item $generatedMatch -Force }
        if (Test-Path $generatedTiming) { Remove-Item $generatedTiming -Force }

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
    }
}
finally {
    Pop-Location
}

Write-Host "`nBatch run complete. Results saved to: $outDir"
