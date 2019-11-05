#!/bin/bash
set -e

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<-EOSQL
	CREATE USER kraho with password 'test';
	ALTER USER kraho superuser;
	CREATE DATABASE testdb owner kraho;
EOSQL
