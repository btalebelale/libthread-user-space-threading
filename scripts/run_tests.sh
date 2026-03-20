#!/bin/bash
set -e

export LD_LIBRARY_PATH=.

echo "===== BASIC TESTS ====="
./tests/01-main
./tests/02-switch
./tests/03-equity

echo "===== JOIN TESTS ====="
./tests/11-join
./tests/12-join-main
./tests/13-join-switch

echo "===== CREATE TESTS ====="
./tests/21-create-many 1000
./tests/22-create-many-recursive 500
./tests/23-create-many-once 1000

echo "===== SWITCH TESTS ====="
./tests/31-switch-many 100 100
./tests/32-switch-many-join 100 100
./tests/33-switch-many-cascade 50 50

echo "===== FIBO ====="
./tests/51-fibonacci 15

echo "===== MUTEX ====="
./tests/61-mutex 100
./tests/62-mutex 100
./tests/63-mutex-equity
./tests/64-mutex-join

echo "===== PREEMPTION ====="
./tests/71-preemption 100

echo "===== DEADLOCK ====="
./tests/81-deadlock