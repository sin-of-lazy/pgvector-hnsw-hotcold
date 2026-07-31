-- HNSW hot/cold benchmark: dataset + index + ground truth
-- Idempotent: safe to re-run.
--
-- Parameters (override on the command line via psql -v):
--   :n_rows          number of base vectors (default 100000)
--   :dim             embedding dimension  (default 128)
--   :n_queries       number of probe queries (default 100)
--   :top_k           recall @K (default 10)
--   :hnsw_m          HNSW m parameter (default 16)
--   :hnsw_efc        HNSW ef_construction (default 64)

\set ON_ERROR_STOP on
\timing off

\if :{?n_rows}    \else \set n_rows 100000 \endif
\if :{?dim}       \else \set dim 128       \endif
\if :{?n_queries} \else \set n_queries 100 \endif
\if :{?top_k}     \else \set top_k 10      \endif
\if :{?hnsw_m}    \else \set hnsw_m 16     \endif
\if :{?hnsw_efc}  \else \set hnsw_efc 64   \endif

CREATE EXTENSION IF NOT EXISTS vector;

-- Deterministic dataset via setseed
SELECT setseed(0.42);

DROP TABLE IF EXISTS bench_items;
CREATE TABLE bench_items (
    id     integer PRIMARY KEY,
    embedding vector(:dim)
);

INSERT INTO bench_items
SELECT g,
       ARRAY(SELECT random() FROM generate_series(1, :dim) i WHERE i > 0 OR g = g)::vector
FROM   generate_series(1, :n_rows) g;

SELECT setseed(0.7);

DROP TABLE IF EXISTS bench_queries;
CREATE TABLE bench_queries (
    qid integer PRIMARY KEY,
    q   vector(:dim)
);

INSERT INTO bench_queries
SELECT g,
       ARRAY(SELECT random() FROM generate_series(1, :dim) i WHERE i > 0 OR g = g)::vector
FROM   generate_series(1, :n_queries) g;

ANALYZE bench_items;
ANALYZE bench_queries;

-- HNSW index (build off the hot path so timings below reflect scan only)
DROP INDEX IF EXISTS bench_items_hnsw;
CREATE INDEX bench_items_hnsw
    ON bench_items
 USING hnsw (embedding vector_l2_ops)
  WITH (m = :hnsw_m, ef_construction = :hnsw_efc);

VACUUM (ANALYZE) bench_items;

-- Ground truth: exact top-K via seqscan (index disabled).
-- Stored so recall computation is a cheap set intersection.
DROP TABLE IF EXISTS bench_truth;
CREATE TABLE bench_truth (
    qid  integer,
    rank integer,
    id   integer,
    PRIMARY KEY (qid, rank)
);

SET enable_indexscan = off;
SET enable_bitmapscan = off;

INSERT INTO bench_truth (qid, rank, id)
SELECT qid,
       row_number() OVER (PARTITION BY qid ORDER BY dist) AS rank,
       id
FROM (
    SELECT q.qid,
           b.id,
           b.embedding <-> q.q AS dist
    FROM   bench_queries q
    CROSS  JOIN LATERAL (
        SELECT id, embedding
        FROM   bench_items
        ORDER  BY embedding <-> q.q
        LIMIT  :top_k
    ) b
) s;

RESET enable_indexscan;
RESET enable_bitmapscan;

\echo Dataset ready.
SELECT COUNT(*) AS items    FROM bench_items;
SELECT COUNT(*) AS queries  FROM bench_queries;
SELECT COUNT(*) AS truth_rows FROM bench_truth;
