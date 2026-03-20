#!/bin/bash
set -e

export LD_LIBRARY_PATH=.

VALGRIND="valgrind --leak-check=full --show-reachable=yes --track-origins=yes"

echo "===== BASIC ====="
$VALGRIND ./01-main
$VALGRIND ./02-switch
$VALGRIND ./03-equity

echo "===== JOIN ====="
$VALGRIND ./11-join
$VALGRIND ./12-join-main
$VALGRIND ./13-join-switch

echo "===== CREATE ====="
$VALGRIND ./21-create-many 100
$VALGRIND ./22-create-many-recursive 200
$VALGRIND ./23-create-many-once 100

echo "===== SWITCH ====="
$VALGRIND ./31-switch-many 20 20
$VALGRIND ./32-switch-many-join 20 20
$VALGRIND ./33-switch-many-cascade 10 10

echo "===== FIBO ====="
$VALGRIND ./51-fibonacci 10

echo "===== MUTEX ====="
$VALGRIND ./61-mutex 20
$VALGRIND ./62-mutex 20
$VALGRIND ./63-mutex-equity
$VALGRIND ./64-mutex-join

echo "===== PREEMPTION ====="
$VALGRIND ./71-preemption 20

echo "===== DEADLOCK ====="
$VALGRIND ./81-deadlock