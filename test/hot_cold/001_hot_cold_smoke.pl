use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node_hot_cold_smoke');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vector;');

my $guc_count = $node->safe_psql('postgres', q(
	SELECT count(*)
	FROM pg_settings
	WHERE name IN (
		'hnsw.hot_cold_enabled',
		'hnsw.hot_layer',
		'hnsw.hot_max_bytes',
		'hnsw.prefetch_neighbors'
	)
));

if ($guc_count != 4)
{
	plan skip_all => 'hot/cold GUCs not available yet; skipping smoke test';
	exit;
}

$node->safe_psql('postgres', q(
	CREATE TABLE tst (i int4, v vector(3));
	INSERT INTO tst
	SELECT i, ARRAY[random(), random(), random()]
	FROM generate_series(1, 5000) i;
	CREATE INDEX idx_tst_hnsw ON tst USING hnsw (v vector_l2_ops);
));

my $explain = $node->safe_psql('postgres', q(
	SET enable_seqscan = off;
	SET hnsw.ef_search = 80;
	SET hnsw.hot_cold_enabled = on;
	SET hnsw.hot_layer = 2;
	SET hnsw.hot_max_bytes = '64MB';
	SET hnsw.prefetch_neighbors = 16;
	EXPLAIN ANALYZE
	SELECT i FROM tst ORDER BY v <-> '[0.1,0.2,0.3]' LIMIT 10;
));

like($explain, qr/Index Scan using idx_tst_hnsw on tst/, 'uses hnsw index scan path');

my $count = $node->safe_psql('postgres', q(
	SET enable_seqscan = off;
	SET hnsw.ef_search = 80;
	SET hnsw.hot_cold_enabled = on;
	SELECT count(*)
	FROM (
		SELECT i FROM tst ORDER BY v <-> '[0.1,0.2,0.3]' LIMIT 10
	) s;
));

is($count, '10', 'returns 10 rows for top-k query');

done_testing();

