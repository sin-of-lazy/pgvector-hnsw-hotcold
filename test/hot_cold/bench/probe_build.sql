-- HNSW build-time observation probe (Phase 3)
--
-- Measures wall-clock, buffer behavior and I/O footprint of CREATE INDEX
-- under different (m, ef_construction, maintenance_work_mem) configurations.
--
-- Usage:
--   psql -f probe_build.sql \
--        -v rows=100000 -v dim=128 \
--        -v m=16 -v efc=64 -v mem='256MB'
--
-- Output: one row per build with elapsed_ms, index_size and buffer counters.

\set ON_ERROR_STOP on

\if :{?rows} \else \set rows 100000 \endif
\if :{?dim}  \else \set dim  128    \endif
\if :{?m}    \else \set m    16     \endif
\if :{?efc}  \else \set efc  64     \endif
\if :{?mem}  \else \set mem  '256MB' \endif

CREATE EXTENSION IF NOT EXISTS vector;

-- Fresh table each run so build time is comparable.
DROP TABLE IF EXISTS build_probe;
CREATE TABLE build_probe (id integer PRIMARY KEY, embedding vector(:dim));

SELECT setseed(0.42);
INSERT INTO build_probe
SELECT g, ARRAY(SELECT random() FROM generate_series(1, :dim) i WHERE i > 0 OR g = g)::vector
FROM   generate_series(1, :rows) g;

ANALYZE build_probe;

-- Reset per-relation and shared counters right before the build.
SELECT pg_stat_reset_single_table_counters('build_probe'::regclass);
SELECT pg_stat_reset_shared('io');

SET maintenance_work_mem = :'mem';

-- Timed build. We rely on \timing / clock_timestamp difference.
SELECT clock_timestamp() AS t0 \gset

CREATE INDEX build_probe_hnsw
    ON build_probe
 USING hnsw (embedding vector_l2_ops)
  WITH (m = :m, ef_construction = :efc);

SELECT clock_timestamp() AS t1 \gset

-- Summary row: elapsed, index size, IO context stats collected during build.
SELECT
    :rows                                            AS rows,
    :dim                                             AS dim,
    :m                                               AS m,
    :efc                                             AS ef_construction,
    :'mem'                                           AS maintenance_work_mem,
    ROUND(EXTRACT(EPOCH FROM (:'t1'::timestamptz - :'t0'::timestamptz)) * 1000)::int AS elapsed_ms,
    pg_size_pretty(pg_relation_size('build_probe_hnsw')) AS index_size,
    pg_relation_size('build_probe_hnsw')             AS index_bytes;

-- Per-context IO stats since the reset above (Postgres 16+).
-- Falls back gracefully on older versions.
SELECT backend_type, context, object,
       reads, writes, extends, hits, evictions
FROM   pg_stat_io
WHERE  backend_type IN ('client backend', 'parallel worker')
   AND (reads > 0 OR writes > 0 OR extends > 0 OR hits > 100)
ORDER  BY (reads + writes + extends) DESC
LIMIT  20;
