use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node_hot_cold_recall');
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
	plan skip_all => 'hot/cold GUCs not available yet; skipping recall test';
	exit;
}

$node->safe_psql('postgres', q(
	CREATE TABLE tst (i int4, v vector(3));
	INSERT INTO tst
	SELECT i, ARRAY[random(), random(), random()]
	FROM generate_series(1, 30000) i;
	CREATE INDEX idx_tst_hnsw ON tst USING hnsw (v vector_l2_ops);
));

my @queries = (
	'[0.11,0.22,0.33]',
	'[0.23,0.34,0.45]',
	'[0.35,0.46,0.57]',
	'[0.47,0.58,0.69]',
	'[0.59,0.70,0.81]'
);

my $limit = 10;

sub recall_for_mode
{
	my ($hot_cold_mode) = @_;
	my $correct = 0;
	my $total = 0;

	for my $q (@queries)
	{
		my $expected = $node->safe_psql('postgres', qq(
			SET enable_indexscan = off;
			SELECT i
			FROM tst
			ORDER BY v <-> '$q'
			LIMIT $limit;
		));

		my $actual = $node->safe_psql('postgres', qq(
			SET enable_seqscan = off;
			SET hnsw.ef_search = 80;
			SET hnsw.hot_cold_enabled = $hot_cold_mode;
			SET hnsw.hot_layer = 2;
			SET hnsw.hot_max_bytes = '64MB';
			SET hnsw.prefetch_neighbors = 16;
			SELECT i
			FROM tst
			ORDER BY v <-> '$q'
			LIMIT $limit;
		));

		my %expected_set = map { $_ => 1 } split(/\n/, $expected);
		for my $id (split(/\n/, $actual))
		{
			$correct++ if exists $expected_set{$id};
		}
		$total += $limit;
	}

	return $correct / $total;
}

my $recall_off = recall_for_mode('off');
my $recall_on = recall_for_mode('on');

cmp_ok($recall_on, '>=', 0.95, 'recall@10 in hot/cold mode is >= 0.95');
cmp_ok($recall_on + 0.05, '>=', $recall_off, 'hot/cold recall is not significantly worse than baseline');

diag(sprintf('recall_off=%.4f recall_on=%.4f', $recall_off, $recall_on));

done_testing();

