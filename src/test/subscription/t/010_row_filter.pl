# Teste logical replication behavior with row filtering
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 4;

# create publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# setup structure on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_1 (a int primary key, b text)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_2 (c int primary key)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_3 (a int primary key, b boolean)");

# setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_1 (a int primary key, b text)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_2 (c int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_3 (a int primary key, b boolean)");

# setup logical replication
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_1 FOR TABLE tab_rowfilter_1 WHERE (a > 1000 AND b <> 'filtered')");

my $result = $node_publisher->psql('postgres',
	"ALTER PUBLICATION tap_pub_1 DROP TABLE tab_rowfilter_1 WHERE (a > 1000 AND b <> 'filtered')");
is($result, 3, "syntax error for ALTER PUBLICATION DROP TABLE");

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_1 ADD TABLE tab_rowfilter_2 WHERE (c % 2 = 0)");

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_1 SET TABLE tab_rowfilter_1 WHERE (a > 1000 AND b <> 'filtered'), tab_rowfilter_2 WHERE (c % 2 = 0), tab_rowfilter_3");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_2 FOR TABLE tab_rowfilter_2 WHERE (c % 3 = 0)");

# test row filtering
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1, 'not replicated')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1500, 'filtered')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1980, 'not filtered')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) SELECT x, 'test ' || x FROM generate_series(990,1003) x");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_2 (c) SELECT generate_series(1, 10)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_3 (a, b) SELECT x, (x % 3 = 0) FROM generate_series(1, 10) x");

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_1, tap_pub_2"
);

$node_publisher->wait_for_catchup($appname);

# wait for initial table sync to finish
my $synced_query =
"SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

#$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT a, b FROM tab_rowfilter_1");
is($result, qq(1980|not filtered
1001|test 1001
1002|test 1002
1003|test 1003), 'check filtered data was copied to subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(c), min(c), max(c) FROM tab_rowfilter_2");
is($result, qq(7|2|10), 'check filtered data was copied to subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(a) FROM tab_rowfilter_3");
is($result, qq(10), 'check filtered data was copied to subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');
