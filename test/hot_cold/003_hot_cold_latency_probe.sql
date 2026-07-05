-- Manual latency probe for HNSW hot/cold mode
-- Prereq:
--   1) CREATE EXTENSION vector;
--   2) table tst(i int, v vector(3)) populated
--   3) index idx on tst using hnsw (v vector_l2_ops)

\timing on
SET enable_seqscan = off;
SET hnsw.ef_search = 80;

\echo ===== hot_cold OFF =====
SET hnsw.hot_cold_enabled = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.1,0.2,0.3]' LIMIT 10;

\echo ===== hot_cold ON =====
SET hnsw.hot_cold_enabled = on;
SET hnsw.hot_layer = 2;
SET hnsw.hot_max_bytes = '64MB';
SET hnsw.prefetch_neighbors = 16;
EXPLAIN (ANALYZE, BUFFERS)
SELECT i FROM tst ORDER BY v <-> '[0.1,0.2,0.3]' LIMIT 10;

