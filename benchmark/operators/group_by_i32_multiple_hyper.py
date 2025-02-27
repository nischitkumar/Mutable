#!/bin/env python3

import sys
sys.path.insert(0, 'benchmark')

import os
import time
from tableauhyperapi import HyperProcess, Telemetry, Connection, CreateMode, NOT_NULLABLE, NULLABLE, SqlType, \
        TableDefinition, Inserter, escape_name, escape_string_literal, HyperException, TableName

import hyperconf

if __name__ == '__main__':
    hyperconf.init() # prepare for measurements
    with HyperProcess(telemetry=Telemetry.DO_NOT_SEND_USAGE_DATA_TO_TABLEAU) as hyper:
        with Connection(endpoint=hyper.endpoint, database='benchmark.hyper', create_mode=CreateMode.CREATE_AND_REPLACE) as connection:
            table_def = TableDefinition(
                table_name='Distinct_i32',
                columns=[
                    TableDefinition.Column('id',      SqlType.int(), NOT_NULLABLE),
                    TableDefinition.Column('n1',      SqlType.int(), NOT_NULLABLE),
                    TableDefinition.Column('n10',     SqlType.int(), NOT_NULLABLE),
                    TableDefinition.Column('n100',    SqlType.int(), NOT_NULLABLE),
                    TableDefinition.Column('n1000',   SqlType.int(), NOT_NULLABLE),
                    TableDefinition.Column('n10000',  SqlType.int(), NOT_NULLABLE),
                    TableDefinition.Column('n100000', SqlType.int(), NOT_NULLABLE),
                ]
            )

            queries = [
                f'SELECT COUNT(*) FROM (SELECT 1 FROM {table_def.table_name} GROUP BY n10000)                   AS T',
                f'SELECT COUNT(*) FROM (SELECT 1 FROM {table_def.table_name} GROUP BY n10000, n1000)            AS T',
                f'SELECT COUNT(*) FROM (SELECT 1 FROM {table_def.table_name} GROUP BY n10000, n1000, n100)      AS T',
                f'SELECT COUNT(*) FROM (SELECT 1 FROM {table_def.table_name} GROUP BY n10000, n1000, n100, n10) AS T',
            ]

            #  queries = [
            #      f'SELECT COUNT(DISTINCT n10000)                   FROM {table_def.table_name} AS T',
            #      f'SELECT COUNT(DISTINCT CONCAT(n10000, n1000))            FROM {table_def.table_name} AS T',
            #      f'SELECT COUNT(DISTINCT CONCAT(n10000, n1000, n100))      FROM {table_def.table_name} AS T',
            #      f'SELECT COUNT(DISTINCT CONCAT(n10000, n1000, n100, n10)) FROM {table_def.table_name} AS T',
            #  ]

            times = hyperconf.benchmark_execution_times(connection, queries, [
                    (table_def, 'benchmark/operators/data/Distinct_i32.csv', { 'FORMAT': 'csv', 'DELIMITER': "','", 'HEADER': 1 })
            ])

            print('\n'.join(map(lambda t: f'{t:.3f}', times)))
