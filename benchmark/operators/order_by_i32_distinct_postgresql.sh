#!/bin/bash

# Define path to PostgreSQL CLI
POSTGRESQL=psql
CSV='benchmark/operators/data/Distinct_i32.csv'

{ ${POSTGRESQL} -U postgres | grep 'Time' | cut -d ' ' -f 2; } << EOF
DROP DATABASE IF EXISTS benchmark_tmp;
CREATE DATABASE benchmark_tmp;
\c benchmark_tmp
set jit=off;
CREATE TABLE Distinct_i32 ( id INT, n1 INT, n10 INT, n100 INT, n1000 INT, n10000 INT, n100000 INT);
\copy Distinct_i32 FROM '${CSV}' WITH DELIMITER ',' CSV HEADER;
\timing on
SELECT id FROM Distinct_i32 ORDER BY     n10;
SELECT id FROM Distinct_i32 ORDER BY    n100;
SELECT id FROM Distinct_i32 ORDER BY   n1000;
SELECT id FROM Distinct_i32 ORDER BY  n10000;
SELECT id FROM Distinct_i32 ORDER BY n100000;
EOF
