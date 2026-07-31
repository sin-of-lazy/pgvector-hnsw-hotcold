# HNSW hot/cold benchmark driver
#
# Usage:
#   $env:PGPASSWORD = "..."
#   .\run_bench.ps1 -Rows 100000 -Dim 128 -Queries 100 -TopK 10
#
# What it does:
#   1) rebuild bench_items / bench_queries / bench_truth (deterministic seed)
#   2) sweep (hot_cold in {off, on}) x (ef_search in {40, 80, 120})
#   3) collect recall + latency + P50/P95/P99 into a CSV under $OutDir
#
# Notes:
#   Not a rigorous benchmark harness; buffers are warmed once per config,
#   PG runs on Windows so timing has ~ms jitter. Results are directional.

[CmdletBinding()]
param(
    [string]$PgBin      = "F:\postgresql\bin",
    [string]$Database   = "postgres",
    [string]$User       = "postgres",
    [int]$Rows          = 100000,
    [int]$Dim           = 128,
    [int]$Queries       = 100,
    [int]$TopK          = 10,
    [int]$HnswM         = 16,
    [int]$HnswEfc       = 64,
    [int]$Warmup        = 1,
    [int[]]$EfSearch    = @(40, 80, 120),
    [string[]]$HotCold  = @('off', 'on'),
    [int]$PrefetchNeighbors = 16,
    [int]$HotLayer      = 2,
    [switch]$SkipSetup,
    [string]$OutDir     = "F:\_WORK\PgVector\test\hot_cold\bench\out"
)

$ErrorActionPreference = 'Stop'
$psql = Join-Path $PgBin 'psql.exe'
if (-not (Test-Path $psql)) { throw "psql not found at $psql" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
$csv     = Join-Path $OutDir "bench_$stamp.csv"
$logFile = Join-Path $OutDir "bench_$stamp.log"

$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$setup   = Join-Path $here 'setup.sql'
$runOne  = Join-Path $here 'run_one.sql'

function Invoke-Psql {
    param([string]$File, [hashtable]$Vars, [switch]$Quiet)
    $args = @('-h', 'localhost', '-U', $User, '-d', $Database, '-X', '-q', '-P', 'pager=off',
              '--set=ON_ERROR_STOP=1')
    foreach ($k in $Vars.Keys) {
        $args += "--set=$k=$($Vars[$k])"
    }
    $args += @('-f', $File)
    if ($Quiet) { & $psql @args | Out-Null } else { & $psql @args }
    if ($LASTEXITCODE -ne 0) { throw "psql failed with code $LASTEXITCODE for $File" }
}

if (-not $SkipSetup) {
    Write-Host "[setup] rebuilding dataset ($Rows rows, dim=$Dim, queries=$Queries) ..."
    Invoke-Psql -File $setup -Vars @{
        n_rows    = $Rows
        dim       = $Dim
        n_queries = $Queries
        top_k     = $TopK
        hnsw_m    = $HnswM
        hnsw_efc  = $HnswEfc
    } -Quiet
    Write-Host "[setup] done."
} else {
    Write-Host "[setup] skipped (using existing bench_items/bench_queries/bench_truth)."
}

"config,ef_search,n_queries,recall_avg,latency_avg_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms" |
    Set-Content -Encoding UTF8 -Path $csv

foreach ($mode in $HotCold) {
    foreach ($ef in $EfSearch) {
        $label = "hot_cold=$mode ef_search=$ef"
        Write-Host "[run] $label"

        $tmpOut = Join-Path $OutDir "tmp_run.txt"
        & $psql -h localhost -U $User -d $Database -X -q -P pager=off `
            --set=ON_ERROR_STOP=1 `
            --set="top_k=$TopK" `
            --set="ef_search=$ef" `
            --set="hot_cold=$mode" `
            --set="hot_layer=$HotLayer" `
            --set="prefetch_neighbors=$PrefetchNeighbors" `
            --set="warmup=$Warmup" `
            -A -F ',' -t -f $runOne 2>&1 |
            Tee-Object -FilePath $logFile -Append |
            Set-Content -Path $tmpOut

        # Last non-empty line is the summary row (psql -A -t -F ',' emits CSV-ish).
        $line = Get-Content $tmpOut | Where-Object { $_ -match '\S' } | Select-Object -Last 1
        if (-not $line) { throw "no output for $label" }

        # Expected columns from run_one.sql:
        # hot_cold,ef_search,top_k,n_queries,recall_avg,latency_avg_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms
        $cols = $line -split ','
        if ($cols.Count -lt 11) { throw "unexpected summary line: $line" }

        "$mode,$($cols[1]),$($cols[3]),$($cols[4]),$($cols[5]),$($cols[6]),$($cols[7]),$($cols[8]),$($cols[9]),$($cols[10])" |
            Add-Content -Encoding UTF8 -Path $csv
    }
}

Remove-Item (Join-Path $OutDir "tmp_run.txt") -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== summary ==="
Import-Csv $csv | Format-Table -AutoSize

Write-Host ""
Write-Host "CSV : $csv"
Write-Host "Log : $logFile"
