# HNSW Hot/Cold Optimization Tests

This folder contains focused tests for the Buffer Pool based HNSW hot/cold optimization.

## Test goals

- Verify feature gating and basic query correctness
- Verify recall is preserved after enabling hot/cold mode
- Provide manual SQL probes for latency and buffer behavior

## Files

- `001_hot_cold_smoke.pl` - smoke test for GUCs, index scan path, and query execution
- `002_hot_cold_recall.pl` - recall comparison between `hnsw.hot_cold_enabled = off/on`
- `003_hot_cold_latency_probe.sql` - manual latency probe with `EXPLAIN (ANALYZE, BUFFERS)`
- `004_hot_cold_buffers_probe.sql` - manual buffer hit/read probe for repeated queries

## How to run

Run from repository root:

```powershell
perl test/hot_cold/001_hot_cold_smoke.pl
perl test/hot_cold/002_hot_cold_recall.pl
```

Manual probes (after creating your own table/index):

```powershell
psql -d postgres -f test/hot_cold/003_hot_cold_latency_probe.sql
psql -d postgres -f test/hot_cold/004_hot_cold_buffers_probe.sql
```

## Notes

- Automated tests are self-skipping when the new hot/cold GUCs are not present yet.
- The recall thresholds are intentionally conservative for early iterations.

