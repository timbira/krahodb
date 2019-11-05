#!/bin/bash

if [ "$HOSTNAME" == "kraho1" ]; then
	pgbench -i -s 5 -h kraho1 -U kraho testdb
	pg_dump -s -h kraho1 -U kraho testdb | psql -f - -h kraho2 -U kraho testdb
	pg_dump -s -h kraho1 -U kraho testdb | psql -f - -h kraho3 -U kraho testdb
	psql -h kraho1 -U kraho -d testdb <<-EOSQL
	CREATE PUBLICATION pub_a_to_b FOR TABLE pgbench_branches WHERE (bid = 2), pgbench_tellers WHERE (bid = 2), pgbench_accounts WHERE (bid = 2);	
	CREATE PUBLICATION pub_a_to_c FOR TABLE pgbench_branches WHERE (bid = 3), pgbench_tellers WHERE (bid = 3), pgbench_accounts WHERE (bid = 3);	
	EOSQL
	psql -h kraho2 -U kraho -d testdb <<-EOSQL
CREATE SUBSCRIPTION sub_b_from_a CONNECTION 'host=kraho1 user=kraho dbname=testdb password=test' PUBLICATION pub_a_to_b WITH (replication_origin_id = 2, filter_origins = 102); 
EOSQL
        psql -h kraho3 -U kraho -d testdb <<-EOSQL
CREATE SUBSCRIPTION sub_c_from_a CONNECTION 'host=kraho1 user=kraho dbname=testdb password=test' PUBLICATION pub_a_to_c WITH (replication_origin_id = 3, filter_origins = 103);
	EOSQL
	psql -h kraho2 -U kraho -d testdb -c 'CREATE PUBLICATION pub_b_to_a FOR TABLE pgbench_branches, pgbench_tellers, pgbench_accounts;'
	psql -h kraho3 -U kraho -d testdb -c 'CREATE PUBLICATION pub_c_to_a FOR TABLE pgbench_branches, pgbench_tellers, pgbench_accounts;'
	psql -h kraho1 -U kraho -d testdb <<-EOSQL
	CREATE SUBSCRIPTION sub_a_from_b CONNECTION 'host=kraho2 user=kraho dbname=testdb password=test' PUBLICATION pub_b_to_a WITH (replication_origin_id = 102, filter_origins = 2, copy_data = false);
	CREATE SUBSCRIPTION sub_a_from_c CONNECTION 'host=kraho3 user=kraho dbname=testdb password=test' PUBLICATION pub_c_to_a WITH (replication_origin_id = 103, filter_origins = 3, copy_data = false);
	EOSQL
fi
