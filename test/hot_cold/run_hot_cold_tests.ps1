$ErrorActionPreference = 'Stop'

Write-Host 'Running HNSW hot/cold test suite...'

perl test/hot_cold/001_hot_cold_smoke.pl
perl test/hot_cold/002_hot_cold_recall.pl

Write-Host 'Done.'

