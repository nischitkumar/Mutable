#!/bin/bash

# Define path to DuckDB CLI
DUCKDB=duckdb_cli
CSV='benchmark/operators/data/Distinct_i32.csv'

{ ${DUCKDB} | grep 'Run Time' | cut -d ' ' -f 4 | awk '{print $1 * 1000;}'; } << EOF
CREATE TABLE Distinct_i32 ( id INT, n1 INT, n10 INT, n100 INT, n1000 INT, n10000 INT, n100000 INT);
COPY Distinct_i32 FROM '${CSV}' ( HEADER );
.timer on
SELECT id FROM Distinct_i32 ORDER BY n100000;
.timer off
DROP TABLE Distinct_i32;

CREATE TABLE Distinct_i32 ( id INT, n1 INT, n10 INT, n100 INT, n1000 INT, n10000 INT, n100000 INT);
COPY Distinct_i32 FROM '${CSV}' ( HEADER );
.timer on
SELECT id FROM Distinct_i32 ORDER BY n10000, n1000;
.timer off
DROP TABLE Distinct_i32;

CREATE TABLE Distinct_i32 ( id INT, n1 INT, n10 INT, n100 INT, n1000 INT, n10000 INT, n100000 INT);
COPY Distinct_i32 FROM '${CSV}' ( HEADER );
.timer on
SELECT id FROM Distinct_i32 ORDER BY n10000, n1000, n100;
.timer off
DROP TABLE Distinct_i32;


CREATE TABLE Distinct_i32 ( id INT, n1 INT, n10 INT, n100 INT, n1000 INT, n10000 INT, n100000 INT);
COPY Distinct_i32 FROM '${CSV}' ( HEADER );
.timer on
SELECT id FROM Distinct_i32 ORDER BY n10000, n1000, n100, n10;
.timer off
DROP TABLE Distinct_i32;
EOF
