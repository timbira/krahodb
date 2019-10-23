# KrahoDB

KrahoDB is an open-source database designed to support multi-master replication. It is designed on the top of PostgreSQL, providing bidirectional replication, as well as row filtering.

KrahoDB is a PostgreSQL fork. We provide versions based on PostgreSQL 10, 11 and 12. All code is licensed under PostgreSQL license.

![Logo](krahodb.png)

## Installation

The [installation process](https://www.postgresql.org/docs/current/installation.html) is the same as PostgreSQL.

Use one of the branches named `krahodb-X-Y` where `X.Y` is the PostgreSQL version (branch `krahodb-10-10` is based on PostgreSQL `10.10`).

The following commands build KrahoDB:

```
$ wget -c https://github.com/timbira/krahodb/archive/KDB_12_0_1.tar.gz
$ tar zxf KDB_12_0_1.tar.gz
$ cd krahodb-KDB_12_0_1
$ ./configure --prefix=$HOME/krahodb --enable-debug
$ make -s -j 4
$ make -s -j 4 install
$ cd contrib
$ make -s -j 4
$ make -s -j 4 install
```

## Usage

Let's suppose we have a setup with 4 nodes: node A (10.20.30.1), node B (10.20.30.2), node C (10.20.30.3), and node D (10.20.30.4). We use the same database (`testdb`) and the same role (`kraho`) although they can be different in different nodes (don't forget to adjust permissions).

```
+------------+          +------------+          +------------+
|    B       |          |    A       |          |    C       |
| 10.20.30.2 |<-------->| 10.20.30.1 |<-------->| 10.20.30.3 |
+------------+          +------------+          +------------+
                             ^
                             |
                             v
                        +------------+
                        |     D      |
                        | 10.20.30.4 |
                        +------------+
```

Setup is the same as [PostgreSQL](https://www.postgresql.org/docs/current/logical-replication.html) with a little differences that we show above.

Adjust `listen_addresses` to allow connections from other nodes:

```
listen_addresses = '*'
```

All nodes must adjust `wal_level` in `postgresql.conf`:

```
wal_level = logical
```

Default replication parameters is sufficient for fewer nodes (don't worry with additional parameters if you have less than 10 nodes). However, if you have a dozen of nodes, node A should adjust `max_wal_senders`, `max_replication_slots` and `max_logical_replication_workers` (at least the same as leaf nodes). Additionally, `max_worker_processes` must be set to accommodate one worker per leaf node.

Node A should receive connections from nodes B, C and D so adjust `pg_hba.conf` in node A:

```
host	testdb		kraho		10.20.30.2/32		md5
host	testdb		kraho		10.20.30.3/32		md5
host	testdb		kraho		10.20.30.4/32		md5
```

Nodes B, C and D should receive connection only from node A so adjust `pg_hba.conf`:

```
host	testdb		kraho		10.20.30.1/32		md5
```

Let's create database `testdb` and role `kraho` in each node.

```
createuser -s -h 10.20.30.1 -U postgres kraho
createuser -s -h 10.20.30.2 -U postgres kraho
createuser -s -h 10.20.30.3 -U postgres kraho
createuser -s -h 10.20.30.4 -U postgres kraho

createdb -h 10.20.30.1 -U postgres -O kraho testdb
createdb -h 10.20.30.2 -U postgres -O kraho testdb
createdb -h 10.20.30.3 -U postgres -O kraho testdb
createdb -h 10.20.30.4 -U postgres -O kraho testdb
```

Let's populate some data in node A for testing.

```
pgbench -i -s 5 -h 10.20.30.1 -U kraho testdb
```

Let's copy the schema from node A to nodes B, C and D.

```
pg_dump -s -h 10.20.30.1 -U kraho testdb | psql -f - -h 10.20.30.2 -U kraho testdb
pg_dump -s -h 10.20.30.1 -U kraho testdb | psql -f - -h 10.20.30.3 -U kraho testdb
pg_dump -s -h 10.20.30.1 -U kraho testdb | psql -f - -h 10.20.30.4 -U kraho testdb
```

Let's create publications to send data from node A to nodes B, C and D. It is important to be separate publications because we will define a different filter to tables. Our row filter will be column `bid` which means that node B, C and D will only have data for an specific `bid`.

```
psql -h 10.20.30.1 -U kraho testdb
testdb=# CREATE PUBLICATION pub_a_to_b FOR TABLE pgbench_branches WHERE (bid = 2), pgbench_tellers WHERE (bid = 2), pgbench_accounts WHERE (bid = 2);
CREATE PUBLICATION
testdb=# CREATE PUBLICATION pub_a_to_c FOR TABLE pgbench_branches WHERE (bid = 3), pgbench_tellers WHERE (bid = 3), pgbench_accounts WHERE (bid = 3);
CREATE PUBLICATION
testdb=# CREATE PUBLICATION pub_a_to_d FOR TABLE pgbench_branches WHERE (bid = 4), pgbench_tellers WHERE (bid = 4), pgbench_accounts WHERE (bid = 4);
CREATE PUBLICATION
```

Let's create subscriptions in nodes B, C and D. By default, it copies data (from node A).

```
psql -h 10.20.30.2 -U kraho testdb
testdb=# CREATE SUBSCRIPTION sub_b_from_a CONNECTION 'host=10.20.30.1 user=kraho dbname=testdb' PUBLICATION pub_a_to_b WITH (replication_origin_id = 2, filter_origins = 102);
CREATE SUBSCRIPTION
testdb=# \q
psql -h 10.20.30.3 -U kraho testdb
testdb=# CREATE SUBSCRIPTION sub_c_from_a CONNECTION 'host=10.20.30.1 user=kraho dbname=testdb' PUBLICATION pub_a_to_c WITH (replication_origin_id = 3, filter_origins = 103);
CREATE SUBSCRIPTION
testdb=# \q
psql -h 10.20.30.4 -U kraho testdb
testdb=# CREATE SUBSCRIPTION sub_d_from_a CONNECTION 'host=10.20.30.1 user=kraho dbname=testdb' PUBLICATION pub_a_to_d WITH (replication_origin_id = 4, filter_origins = 104);
CREATE SUBSCRIPTION
testdb=# \q
```

At this point, replication is configured in one direction: A -> B, A -> C and A -> D. Let's configure the other direction.

Let's create publication to each node (B, C, D). Since those nodes only contain data from one branch, do not filter rows.

```
psql -h 10.20.30.2 -U kraho test
testdb=# CREATE PUBLICATION pub_b_to_a FOR TABLE pgbench_branches, pgbench_tellers, pgbench_accounts;
CREATE PUBLICATION
testdb=# \q
psql -h 10.20.30.3 -U kraho test
testdb=# CREATE PUBLICATION pub_c_to_a FOR TABLE pgbench_branches, pgbench_tellers, pgbench_accounts;
CREATE PUBLICATION
testdb=# \q
psql -h 10.20.30.4 -U kraho test
testdb=# CREATE PUBLICATION pub_d_to_a FOR TABLE pgbench_branches, pgbench_tellers, pgbench_accounts;
CREATE PUBLICATION
testdb=# \q
```

Let's create subscriptions in node A. Have in mind that data was already copied from node A to the other nodes. In this case, subscription must indicate that data will not be copied.

```
psql -h 10.20.30.1 -U kraho testdb
testdb=# CREATE SUBSCRIPTION sub_a_from_b CONNECTION 'host=10.20.30.2 user=kraho dbname=testdb' PUBLICATION pub_b_to_a WITH (replication_origin_id = 102, filter_origins = 2, copy_data = false);
CREATE SUBSCRIPTION
testdb=# CREATE SUBSCRIPTION sub_a_from_c CONNECTION 'host=10.20.30.3 user=kraho dbname=testdb' PUBLICATION pub_c_to_a WITH (replication_origin_id = 103, filter_origins = 3, copy_data = false);
CREATE SUBSCRIPTION
testdb=# CREATE SUBSCRIPTION sub_a_from_d CONNECTION 'host=10.20.30.4 user=kraho dbname=testdb' PUBLICATION pub_d_to_a WITH (replication_origin_id = 104, filter_origins = 4, copy_data = false);
CREATE SUBSCRIPTION
testdb=# \q
```

Setup is ready. Let's insert some rows in node A.


```
psql -h 10.20.30.1 -U kraho testdb
testdb=# -- this data will not be replicate to nodes because it does not match row filter
testdb=# INSERT INTO pgbench_branches (bid, bbalance, filler) VALUES(100, 123345, 'KrahoDB');
INSERT 0 1
testdb=# -- this row will be replicated to node B
testdb=# INSERT INTO pgbench_tellers (tid, bid, tbalance, filler) VALUES(23456, 2, 22222, 'KrahoDB - node B');
INSERT 0 1
testdb=# -- this row will be replicated to node C
testdb=# INSERT INTO pgbench_tellers (tid, bid, tbalance, filler) VALUES(34567, 3, 33333, 'KrahoDB - node C');
INSERT 0 1
testdb=# -- this row will be replicated to node D
testdb=# INSERT INTO pgbench_tellers (tid, bid, tbalance, filler) VALUES(45678, 4, 44444, 'KrahoDB - node D');
INSERT 0 1
```

Let's check in the other nodes if data was replicated.

```
psql -h 10.20.30.2 -U kraho testdb
testdb=# SELECT * FROM pgbench_tellers WHERE tid = 23456;
  tid  | bid | tbalance |                                        filler                                        
-------+-----+----------+--------------------------------------------------------------------------------------
 23456 |   2 |    22222 | KrahoDB - node B                                                                    
(1 row)
testdb=# \q
psql -h 10.20.30.3 -U kraho testdb
testdb=# SELECT * FROM pgbench_tellers WHERE tid = 34567;
  tid  | bid | tbalance |                                        filler                                        
-------+-----+----------+--------------------------------------------------------------------------------------
 34567 |   3 |    33333 | KrahoDB - node C                                                                    
(1 row)
testdb=# \q
psql -h 10.20.30.4 -U kraho testdb
testdb=# SELECT * FROM pgbench_tellers WHERE tid = 45678;
  tid  | bid | tbalance |                                        filler                                        
-------+-----+----------+--------------------------------------------------------------------------------------
 45678 |   4 |    44444 | KrahoDB - node D                                                                    
(1 row)
testdb=# \q
```

Let's update rows in nodes B, C and D.

```
psql -h 10.20.30.2 -U kraho testdb
testdb=# -- this row will be replicated to node A
testdb=# UPDATE pgbench_tellers SET tbalance = -1 * tbalance WHERE tid = 23456;
UPDATE 1
testdb=# \q
psql -h 10.20.30.3 -U kraho testdb
testdb=# -- this row will be replicated to node A
testdb=# UPDATE pgbench_tellers SET tbalance = -1 * tbalance WHERE tid = 34567;
UPDATE 1
testdb=# \q
psql -h 10.20.30.4 -U kraho testdb
testdb=# -- this row will be replicated to node A
testdb=# UPDATE pgbench_tellers SET tbalance = -1 * tbalance WHERE tid = 45678;
UPDATE 1
```

Let's check in node A if data was replicated.

```
psql -h 10.20.30.1 -U kraho testdb
testdb=# SELECT * FROM pgbench_tellers WHERE tid IN (23456, 34567, 45678);
testdb=# SELECT * FROM pgbench_tellers WHERE tid IN (23456, 34567, 45678);
  tid  | bid | tbalance |                                        filler                                        
-------+-----+----------+--------------------------------------------------------------------------------------
 23456 |   2 |   -22222 | KrahoDB - node B                                                                    
 34567 |   3 |   -33333 | KrahoDB - node C                                                                    
 45678 |   4 |   -44444 | KrahoDB - node D                                                                    
(3 rows)
```

Let's remove inserted row in nodes B, C and D to test the other direction.

```
psql -h 10.20.30.2 -U kraho testdb
testdb=# DELETE FROM pgbench_tellers WHERE tid = 23456;
DELETE 1
testdb=# \q
psql -h 10.20.30.3 -U kraho testdb
testdb=# DELETE FROM pgbench_tellers WHERE tid = 34567;
DELETE 1
testdb=# \q
psql -h 10.20.30.4 -U kraho testdb
testdb=# DELETE FROM pgbench_tellers WHERE tid = 45678;
DELETE 1
testdb=# \q
```

Let's check in node A if rows were removed.

```
psql -h 10.20.30.1 -U kraho testdb
testdb=# SELECT * FROM pgbench_tellers WHERE tid IN (23456, 34567, 45678);
 tid | bid | tbalance | filler 
-----+-----+----------+--------
(0 rows)
```

## Contributing

Contributions are welcome!

How can you help us?

* open an issue
    - bug report
	- feature request
	- suggestion
	- portability problem
* submit a pull request

Before submitting a particular improvement, open an issue to start a discussion about the feature (specially if it is not a trivial change).

Read the [PostgreSQL Coding Conventions](https://www.postgresql.org/docs/current/source.html). It is what we use for KrahoDB. Also, follow the style of the adjacent code! Remove any spurious whitespace (`git diff --color`). We tend to use underscores or Camelcase (prefer the former). Add useful comments (explain a behavior). Avoid "opens a file" comments.

KrahoDB uses [PostgreSQL license](http://www.postgresql.org/about/licence/). By posting a patch (issue or pull request), you agree that your patch can be distributed under the PostgreSQL license.
