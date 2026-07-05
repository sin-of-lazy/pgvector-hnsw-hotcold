-- Manual buffer probe for repeated queries under off/on modes
-- Run this script multiple times and compare shared/local hit/read counters.

SET enable_seqscan = off;
SET hnsw.ef_search = 80;

\echo ===== warm up =====
SELECT count(*) FROM tst;

\echo ===== mode: OFF =====
SET hnsw.hot_cold_enabled = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.21,0.34,0.55]' LIMIT 20;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.22,0.35,0.56]' LIMIT 20;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.23,0.36,0.57]' LIMIT 20;

\echo ===== mode: ON =====
SET hnsw.hot_cold_enabled = on;
SET hnsw.hot_layer = 2;
SET hnsw.hot_max_bytes = '64MB';
SET hnsw.prefetch_neighbors = 16;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.21,0.34,0.55]' LIMIT 20;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.22,0.35,0.56]' LIMIT 20;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.23,0.36,0.57]' LIMIT 20;

