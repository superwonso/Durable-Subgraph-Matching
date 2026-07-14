[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$sourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $SkipBuild) {
    & (Join-Path $sourceDir "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

$testDir = Join-Path ([System.IO.Path]::GetTempPath()) ("turboiso-tests-" + [guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($testDir) | Out-Null

function Write-Ascii([string]$Path, [string]$Content) {
    Set-Content -LiteralPath $Path -Value $Content.TrimStart() -Encoding ascii -NoNewline
}

function Invoke-Turbo([string]$Data, [string]$Query, [string[]]$Extra = @()) {
    $resultPath = Join-Path $testDir "answers.txt"
    $arguments = @($Data, $Query, $resultPath) + $Extra
    $output = & (Join-Path $sourceDir "turbo.exe") @arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "TurboISO failed:`n$output"
    }
    return $output
}

function Assert-TurboFails(
    [string]$Data,
    [string]$Query,
    [string]$Pattern,
    [string]$Message
) {
    $resultPath = Join-Path $testDir "invalid-answers.txt"
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & (Join-Path $sourceDir "turbo.exe") $Data $Query $resultPath 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -eq 0) {
        throw "$Message`nMalformed input unexpectedly exited zero.`n$output"
    }
    Assert-Match $output $Pattern $Message
}

function Assert-ConverterFails(
    [string]$Temporal,
    [string]$Query,
    [string]$Pattern,
    [string]$Message
) {
    $dataPath = Join-Path $testDir "invalid-converted-data.turbo"
    $queryPath = Join-Path $testDir "invalid-converted-query.turbo"
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & (Join-Path $sourceDir "turbo_convert.exe") `
            $Temporal $Query $dataPath $queryPath 42 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -eq 0) {
        throw "$Message`nMalformed converter input unexpectedly exited zero.`n$output"
    }
    Assert-Match $output $Pattern $Message
}

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) {
        throw "$Message`nExpected pattern: $Pattern`nActual:`n$Text"
    }
}

try {
    $query3 = Join-Path $testDir "query3.turbo"
    Write-Ascii $query3 @"
t # 0
3 2 3 1
v 0 1
v 1 2
v 2 3
e 0 1 1
e 1 2 1
t # -1
"@

    $reverseData = Join-Path $testDir "reverse.turbo"
    Write-Ascii $reverseData @"
t # 0
3 2 3
v 0 1
v 1 2
v 2 3
e 1 0
e 1 2
t # -1
"@
    $reverseOutput = Invoke-Turbo $reverseData $query3
    Assert-Match $reverseOutput 'nembeddings=0(?:\s|$)' "A reversed data arc was accepted as directed."
    Assert-Match $reverseOutput 'timed_out=no' "Default timeout blocked or expired before matching."

    $positiveData = Join-Path $testDir "positive.turbo"
    Write-Ascii $positiveData @"
t # 0
3 2 3
v 0 1
v 1 2
v 2 3
e 0 1
e 1 2
t # -1
"@
    $positiveOutput = Invoke-Turbo $positiveData $query3
    Assert-Match $positiveOutput 'nembeddings=1(?:\s|$)' "A valid directed path was not found exactly once."
    $positiveResult = Get-Content -LiteralPath (Join-Path $testDir "answers.txt") -Raw
    Assert-Match $positiveResult 'Count: 1(?:\s|$)' "The result file did not record the exact count."
    $exactLimitedOutput = Invoke-Turbo $positiveData $query3 @("--max-results", "1")
    Assert-Match $exactLimitedOutput 'nembeddings=1(?:\s|$)' "Exact-size result cap changed the count."
    Assert-Match $exactLimitedOutput 'truncated=no' "An exact-size result cap was falsely reported as truncated."

    $query2 = Join-Path $testDir "query2.turbo"
    Write-Ascii $query2 @"
t # 0
2 1 3 1
v 0 1
v 1 2
e 0 1 1
t # -1
"@
    $twoVertexOutput = Invoke-Turbo $positiveData $query2
    Assert-Match $twoVertexOutput 'nembeddings=1(?:\s|$)' "The two-vertex query failed."

    $reciprocalQuery = Join-Path $testDir "reciprocal-query.turbo"
    Write-Ascii $reciprocalQuery @"
t # 0
2 2 3 1
v 0 1
v 1 2
e 0 1 1
e 1 0 1
t # -1
"@
    $missingReciprocalOutput = Invoke-Turbo $positiveData $reciprocalQuery
    Assert-Match $missingReciprocalOutput 'nembeddings=0(?:\s|$)' "A missing reciprocal arc was accepted."

    $reciprocalData = Join-Path $testDir "reciprocal-data.turbo"
    Write-Ascii $reciprocalData @"
t # 0
3 3 3
v 0 1
v 1 2
v 2 3
e 0 1
e 1 0
e 1 2
t # -1
"@
    $reciprocalOutput = Invoke-Turbo $reciprocalData $reciprocalQuery
    Assert-Match $reciprocalOutput 'nembeddings=1(?:\s|$)' "Valid reciprocal arcs were not retained."

    $query1 = Join-Path $testDir "query1.turbo"
    Write-Ascii $query1 @"
t # 0
1 0 3 1
v 0 1
t # -1
"@
    $oneVertexOutput = Invoke-Turbo $positiveData $query1
    Assert-Match $oneVertexOutput 'nembeddings=1(?:\s|$)' "The one-vertex query failed."

    $multiRootData = Join-Path $testDir "multi-root.turbo"
    Write-Ascii $multiRootData @"
t # 0
4 2 3
v 0 1
v 1 2
v 2 3
v 3 1
e 0 1
e 1 2
t # -1
"@
    $limitedOutput = Invoke-Turbo $multiRootData $query1 @("--max-results", "1")
    Assert-Match $limitedOutput 'nembeddings=1(?:\s|$)' "The explicit result limit returned the wrong count."
    Assert-Match $limitedOutput 'truncated=yes' "The explicit result limit was not reported as truncated."

    $temporal = Join-Path $testDir "temporal.txt"
    $edgeQuery = Join-Path $testDir "query.txt"
    Write-Ascii $temporal @"
1 1 0
2 3 1
2 3 2
3 2 3
"@
    Write-Ascii $edgeQuery @"
A B
"@
    $borrowedTriple = Join-Path $testDir "borrowed-triple.txt"
    Write-Ascii $borrowedTriple @"
1 2
3 4 5 6
"@
    Assert-ConverterFails $borrowedTriple $edgeQuery 'exactly three integers' "Temporal rows borrowed integers across line boundaries."
    $convertedData = Join-Path $testDir "converted-data.turbo"
    $convertedQuery = Join-Path $testDir "converted-query.turbo"
    $convertedData2 = Join-Path $testDir "converted-data-2.turbo"
    $convertedQuery2 = Join-Path $testDir "converted-query-2.turbo"

    & (Join-Path $sourceDir "turbo_convert.exe") $temporal $edgeQuery $convertedData $convertedQuery 42 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Converter failed" }
    & (Join-Path $sourceDir "turbo_convert.exe") $temporal $edgeQuery $convertedData2 $convertedQuery2 42 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Second converter run failed" }

    $convertedText = Get-Content -LiteralPath $convertedData -Raw
    Assert-Match $convertedText '(?m)^2 2 5\r?$' "Timestamp collapse or active vertex compaction is incorrect."
    Assert-Match $convertedText '(?m)^v 0 4\r?$' "Raw self-loop ID did not consume its seed-42 label before active ID 2."
    Assert-Match $convertedText '(?m)^v 1 5\r?$' "Active ID 3 received the wrong seed-42 label."
    Assert-Match $convertedText '(?m)^e 0 1\r?$' "Forward directed arc is missing."
    Assert-Match $convertedText '(?m)^e 1 0\r?$' "Reciprocal directed arc was collapsed incorrectly."
    if ($convertedText -match '(?m)^v 2 ' -or $convertedText -match '(?m)^e 0 0\r?$') {
        throw "Self-loop-only ID was emitted as a Turbo vertex or matching arc."
    }
    if ((Get-FileHash $convertedData).Hash -ne (Get-FileHash $convertedData2).Hash -or
        (Get-FileHash $convertedQuery).Hash -ne (Get-FileHash $convertedQuery2).Hash) {
        throw "Seed-42 conversion is not deterministic."
    }

    $convertedRun = Invoke-Turbo $convertedData $convertedQuery
    Assert-Match $convertedRun 'timed_out=no' "Converted current-format input did not run normally."

    $multiLabelData = Join-Path $testDir "multi-label-data.turbo"
    Write-Ascii $multiLabelData @"
t # 0
3 2 3
v 0 1
v 0 3
v 1 2
v 2 3
e 0 1
e 1 2
t # -1
"@
    $multiLabelOutput = Invoke-Turbo $multiLabelData $query3
    Assert-Match $multiLabelOutput 'nembeddings=1(?:\s|$)' "Distinct labels on one native data vertex were rejected or misread."

    $missingSentinel = Join-Path $testDir "missing-sentinel.turbo"
    Write-Ascii $missingSentinel @"
t # 0
3 2 3
v 0 1
v 1 2
v 2 3
e 0 1
e 1 2
"@
    Assert-TurboFails $missingSentinel $query3 'missing t # -1' "Missing data sentinel was not rejected."

    $badHeader = Join-Path $testDir "bad-header.turbo"
    Write-Ascii $badHeader @"
t # 0
0 0 3
t # -1
"@
    Assert-TurboFails $badHeader $query3 'invalid or missing data graph header' "Non-positive native header was not rejected."

    $duplicateVertexLabel = Join-Path $testDir "duplicate-vertex-label.turbo"
    Write-Ascii $duplicateVertexLabel @"
t # 0
3 2 3
v 0 1
v 0 1
v 1 2
v 2 3
e 0 1
e 1 2
t # -1
"@
    Assert-TurboFails $duplicateVertexLabel $query3 'duplicate data vertex ID/label pair' "Duplicate data vertex label was not rejected."

    $missingVertex = Join-Path $testDir "missing-vertex.turbo"
    Write-Ascii $missingVertex @"
t # 0
3 1 3
v 0 1
v 1 2
e 0 1
t # -1
"@
    Assert-TurboFails $missingVertex $query3 'header counts do not match' "Missing native vertex was not rejected."

    $badVertex = Join-Path $testDir "bad-vertex.turbo"
    Write-Ascii $badVertex @"
t # 0
3 1 3
v 0 1
v 1 4
v 2 3
e 0 1
t # -1
"@
    Assert-TurboFails $badVertex $query3 'invalid data vertex' "Out-of-range native vertex label was not rejected."

    $badEdge = Join-Path $testDir "bad-edge.turbo"
    Write-Ascii $badEdge @"
t # 0
3 1 3
v 0 1
v 1 2
v 2 3
e 0 3
t # -1
"@
    Assert-TurboFails $badEdge $query3 'invalid data edge' "Out-of-range native edge endpoint was not rejected."

    $badQueryLabel = Join-Path $testDir "bad-query-edge-label.turbo"
    Write-Ascii $badQueryLabel @"
t # 0
2 1 3 1
v 0 1
v 1 2
e 0 1 2
t # -1
"@
    Assert-TurboFails $positiveData $badQueryLabel 'invalid query edge' "Out-of-range query edge label was not rejected."

    $duplicateQueryVertex = Join-Path $testDir "duplicate-query-vertex.turbo"
    Write-Ascii $duplicateQueryVertex @"
t # 0
2 1 3 1
v 0 1
v 0 2
v 1 2
e 0 1 1
t # -1
"@
    Assert-TurboFails $positiveData $duplicateQueryVertex 'duplicate query vertex ID' "Duplicate query vertex ID was not rejected."

    $disconnectedQuery = Join-Path $testDir "disconnected-query.turbo"
    Write-Ascii $disconnectedQuery @"
t # 0
3 1 3 1
v 0 1
v 1 2
v 2 3
e 0 1 1
t # -1
"@
    Assert-TurboFails $positiveData $disconnectedQuery 'disconnected query graphs' "Disconnected query did not produce a failing exit status."

    Write-Host "All TurboISO tests passed."
} finally {
    if (Test-Path -LiteralPath $testDir) {
        Remove-Item -LiteralPath $testDir -Recurse -Force
    }
}
