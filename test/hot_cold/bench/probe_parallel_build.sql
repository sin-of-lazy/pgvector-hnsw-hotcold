-- HNSW parallel build lock observation probe (Phase 4)
--
-- Runs CREATE INDEX with varying max_parallel_maintenance_workers to observe
-- how build time scales with parallelism. Also captures wait_event stats
-- to identify lock contention points.
--
-- Usage:
--   psql -f probe_parallel_build.sql \
--        -v rows=50000 -v dim=128 -v m=16 -v efc=64 -v mem='512MB'
--
-- Output:
--   1) Per-parallelism-level build timing
--   2) Wait event snapshot (if locks are contended, LWLock events show up)

\set ON_ERROR_STOP on

\if :{?rows} \else \set rows 50000  \endif
\if :{?dim}  \else \set dim  128    \endif
\if :{?m}    \else \set m    16     \endif
\if :{?efc}  \else \set efc  64     \endif
\if :{?mem}  \else \set mem  512MB  \endif

CREATE EXTENSION IF NOT EXISTS vector;

DROP TABLE IF EXISTS par_build;
CREATE TABLE par_build (id integer PRIMARY KEY, embedding vector(:dim));

SELECT setseed(0.42);
INSERT INTO par_build
SELECT g, ARRAY(SELECT random() FROM generate_series(1, :dim) i WHERE i > 0 OR g = g)::vector
FROM   generate_series(1, :rows) g;
ANALYZE par_build;

SET maintenance_work_mem = :'mem';
SET bench.m   TO :'m';
SET bench.efc TO :'efc';

-- Helper: build index with given parallel workers, return elapsed ms.
CREATE OR REPLACE FUNCTION do_build(workers int) RETURNS TABLE(parallelism int, elapsed_ms numeric) LANGUAGE plpgsql AS $$
DECLARE
    t0 timestamptz;
    t1 timestamptz;
    v_m   int := current_setting('bench.m')::int;
    v_efc int := current_setting('bench.efc')::int;
BEGIN
    EXECUTE format('SET max_parallel_maintenance_workers = %s', workers);
    EXECUTE 'DROP INDEX IF EXISTS par_build_hnsw';
    t0 := clock_timestamp();
    EXECUTE format(
        'CREATE INDEX par_build_hnsw ON par_build USING hnsw (embedding vector_l2_ops) WITH (m = %s, ef_construction = %s)',
        v_m, v_efc
    );
    t1 := clock_timestamp();
    parallelism := workers;
    elapsed_ms := ROUND(EXTRACT(EPOCH FROM (t1 - t0)) * 1000);
    RETURN NEXT;
END $$;

\echo === Parallel build timing sweep ===

SELECT * FROM do_build(0)
UNION ALL SELECT * FROM do_build(1)
UNION ALL SELECT * FROM do_build(2)
UNION ALL SELECT * FROM do_build(4)
ORDER BY parallelism;

-- Wait event snapshot: shows what the backend was waiting on during builds.
-- Useful if lock contention is suspected.
\echo === Wait events (post-build snapshot) ===
SELECT wait_event_type, wait_event, COUNT(*) AS occurrences
FROM   pg_stat_activity
WHERE  state = 'active'
  AND  pid != pg_backend_pid()
GROUP  BY wait_event_type, wait_event
ORDER  BY occurrences DESC
LIMIT  20;

DROP FUNCTION IF EXISTS do_build(int);
