param(
    [string]$ExecutablePath = ".\\td_tree_bfs.exe",
    [string]$DatasetDir = "..\\Dataset",
    [string]$QueryGraph = "..\\Dataset\\Query5.txt",
    [int]$K = 5,
    [string]$OutputDir = ".\\batch_results_bfs_gpu",
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = (Resolve-Path (Join-Path $scriptRoot $ExecutablePath)).Path
$dataDir = (Resolve-Path (Join-Path $scriptRoot $DatasetDir)).Path
$query = (Resolve-Path (Join-Path $scriptRoot $QueryGraph)).Path
$outDir = Join-Path $scriptRoot $OutputDir

if (-not (Test-Path $exe)) {
    throw "Executable not found: $ExecutablePath"
}
if (-not (Test-Path $dataDir)) {
    throw "Dataset directory not found: $DatasetDir"
}
if (-not (Test-Path $query)) {
    throw "Query graph file not found: $QueryGraph"
}

$runner = $exe
if ([System.IO.Path]::GetExtension($exe).ToLowerInvariant() -ne ".exe") {
    $runner = Join-Path $scriptRoot "td_tree_runner.exe"
    $needCopy = $true

    if (Test-Path $runner) {
        $srcTime = (Get-Item $exe).LastWriteTimeUtc
        $dstTime = (Get-Item $runner).LastWriteTimeUtc
        if ($dstTime -ge $srcTime) {
            $needCopy = $false
        }
    }

    if ($needCopy) {
        Copy-Item -Path $exe -Destination $runner -Force
    }
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$datasets = Get-ChildItem -Path $dataDir -File | Where-Object { $_.Name -like "*unique*_filtered.txt" } | Sort-Object Name
if ($datasets.Count -eq 0) {
    Write-Host "No datasets matched pattern '*unique*_filtered.txt' in $dataDir"
    exit 0
}

Write-Host "Found $($datasets.Count) datasets"
Write-Host "Executable : $exe"
Write-Host "Runner     : $runner"
Write-Host "Query graph: $query"
Write-Host "k         : $K"
Write-Host "Output dir: $outDir"

Push-Location $scriptRoot
try {
    foreach ($ds in $datasets) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($ds.Name)
        $stdoutPath = Join-Path $outDir ("{0}__stdout.txt" -f $base)
        $matchOut = Join-Path $outDir ("{0}__matching_results.txt" -f $base)
        $timingOut = Join-Path $outDir ("{0}__timing_results.txt" -f $base)

        Write-Host "`n=== Running: $($ds.Name) ==="
        $cmdPreview = "& `"$runner`" `"$($ds.FullName)`" `"$query`" $K"
        Write-Host $cmdPreview

        if ($DryRun) {
            continue
        }

        if (Test-Path "matching_results.txt") { Remove-Item "matching_results.txt" -Force }
        if (Test-Path "timing_results.txt") { Remove-Item "timing_results.txt" -Force }

        & $runner $ds.FullName $query $K 2>&1 | Tee-Object -FilePath $stdoutPath
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Execution failed for $($ds.Name) (exit code: $LASTEXITCODE)"
            continue
        }

        if (Test-Path "matching_results.txt") {
            Copy-Item "matching_results.txt" $matchOut -Force
        } else {
            Write-Warning "matching_results.txt not produced for $($ds.Name)"
        }

        if (Test-Path "timing_results.txt") {
            Copy-Item "timing_results.txt" $timingOut -Force
        } else {
            Write-Warning "timing_results.txt not produced for $($ds.Name)"
        }
    }
}
finally {
    Pop-Location
}

Write-Host "`nBatch run complete. Results saved to: $outDir"
