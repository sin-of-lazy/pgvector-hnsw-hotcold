-- HNSW hot/cold benchmark: measure recall + latency for one configuration.
--
-- Prereq: run setup.sql once. This script assumes:
--   bench_items, bench_queries, bench_truth exist and are populated.
--
-- Parameters:
--   :top_k                 recall @K (default 10)
--   :ef_search             hnsw.ef_search (default 40)
--   :hot_cold              on | off (default off)
--   :hot_layer             hnsw.hot_layer (default 2)
--   :prefetch_neighbors    hnsw.prefetch_neighbors (default 16)
--   :warmup                number of warmup passes over all queries (default 1)
--
-- Output: one summary row with recall + latency percentiles.

\set ON_ERROR_STOP on

\if :{?top_k}              \else \set top_k 10              \endif
\if :{?ef_search}          \else \set ef_search 40          \endif
\if :{?hot_cold}           \else \set hot_cold off          \endif
\if :{?hot_layer}          \else \set hot_layer 2           \endif
\if :{?prefetch_neighbors} \else \set prefetch_neighbors 16 \endif
\if :{?warmup}             \else \set warmup 1              \endif

SET enable_seqscan  = off;
SET enable_bitmapscan = off;
SET hnsw.ef_search  = :ef_search;
SET hnsw.hot_cold_enabled  = :hot_cold;
SET hnsw.hot_layer         = :hot_layer;
SET hnsw.prefetch_neighbors = :prefetch_neighbors;

-- Pass psql variables into PL/pgSQL via GUC. The `bench.` prefix is a custom
-- namespace so PostgreSQL treats them as unreserved custom parameters.
SET bench.top_k       TO :'top_k';
SET bench.warmup      TO :'warmup';

-- Warmup: pull the working set into buffer pool so measured latencies are
-- stable across runs. Otherwise the first pass dominates the timing.
DO $$
DECLARE
    warm   int := current_setting('bench.warmup')::int;
    k      int := current_setting('bench.top_k')::int;
    r      RECORD;
    dummy  int;
BEGIN
    FOR _w IN 1..warm LOOP
        FOR r IN SELECT q FROM bench_queries LOOP
            EXECUTE format(
                'SELECT id FROM bench_items ORDER BY embedding <-> %L::vector LIMIT %s',
                r.q, k
            );
        END LOOP;
    END LOOP;
END $$;

-- Measured pass: one row per query with latency + recall.
DROP TABLE IF EXISTS bench_run;
CREATE TEMP TABLE bench_run (
    qid       integer,
    latency_ms double precision,
    recall     double precision,
    hit        integer
);

DO $$
DECLARE
    k          int := current_setting('bench.top_k')::int;
    r          RECORD;
    t0         timestamptz;
    t1         timestamptz;
    got        int[];
    truth      int[];
    hit_count  int;
BEGIN
    FOR r IN SELECT qid, q FROM bench_queries ORDER BY qid LOOP
        SELECT array_agg(id) INTO truth
        FROM   bench_truth
        WHERE  qid = r.qid AND rank <= k;

        t0 := clock_timestamp();
        EXECUTE format(
            'SELECT array_agg(id) FROM (SELECT id FROM bench_items ORDER BY embedding <-> %L::vector LIMIT %s) s',
            r.q, k
        ) INTO got;
        t1 := clock_timestamp();

        SELECT COUNT(*) INTO hit_count
        FROM   unnest(got) x
        WHERE  x = ANY(truth);

        INSERT INTO bench_run VALUES (
            r.qid,
            EXTRACT(EPOCH FROM (t1 - t0)) * 1000.0,
            hit_count::double precision / k,
            hit_count
        );
    END LOOP;
END $$;

-- Summary row: one line per invocation. Format matches run_bench.ps1 parser.
SELECT
    current_setting('hnsw.hot_cold_enabled')      AS hot_cold,
    current_setting('hnsw.ef_search')::int        AS ef_search,
    current_setting('bench.top_k')::int           AS top_k,
    COUNT(*)                                      AS n_queries,
    ROUND(AVG(recall)::numeric, 4)                AS recall_avg,
    ROUND(AVG(latency_ms)::numeric, 3)            AS latency_avg_ms,
    ROUND(percentile_cont(0.50) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3) AS p50_ms,
    ROUND(percentile_cont(0.95) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3) AS p95_ms,
    ROUND(percentile_cont(0.99) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3) AS p99_ms,
    ROUND(MIN(latency_ms)::numeric, 3)            AS min_ms,
    ROUND(MAX(latency_ms)::numeric, 3)            AS max_ms
FROM bench_run;
