#!/bin/bash
set -e

export LD_LIBRARY_PATH=.

VALGRIND="valgrind --leak-check=full --show-reachable=yes --track-origins=yes"

echo "===== BASIC (M:N) ====="
$VALGRIND ./tests/01-main
$VALGRIND ./tests/02-switch
$VALGRIND ./tests/03-equity

echo "===== JOIN (M:N) ====="
$VALGRIND ./tests/11-join
$VALGRIND ./tests/12-join-main
$VALGRIND ./tests/13-join-switch

echo "===== CREATE (M:N) ====="
$VALGRIND ./tests/21-create-many 100
$VALGRIND ./tests/22-create-many-recursive 100
$VALGRIND ./tests/23-create-many-once 100

echo "===== SWITCH (M:N) ====="
$VALGRIND ./tests/31-switch-many 20 20
$VALGRIND ./tests/32-switch-many-join 20 20
$VALGRIND ./tests/33-switch-many-cascade 10 10

echo "===== MUTEX (M:N) ====="
$VALGRIND ./tests/61-mutex 20
$VALGRIND ./tests/62-mutex 20
$VALGRIND ./tests/63-mutex-equity
$VALGRIND ./tests/64-mutex-join

echo "===== SEMAPHORES (M:N) ====="
$VALGRIND ./tests/64-sem
$VALGRIND ./tests/65-sem-prodcons

echo "===== DEADLOCK (M:N) ====="
$VALGRIND ./tests/81-deadlock

echo "===== SIGNALS (M:N) ====="
$VALGRIND ./tests/124-signals

echo "===== TESTS MULTICORE ====="
$VALGRIND ./tests/101-multicore-counter 4
$VALGRIND ./tests/102-multicore-concurrent-work
$VALGRIND ./tests/103-multicore-sem-prodcons
$VALGRIND ./tests/104-multicore-mutex-ordering
$VALGRIND ./tests/105-multicore-join-values
$VALGRIND ./tests/106-multicore-cascade-mutex
$VALGRIND ./tests/107-multicore-yield-fairness 4
$VALGRIND ./tests/111-multicore-stress-create-join
$VALGRIND ./tests/112-multicore-stress-mutex
$VALGRIND ./tests/113-multicore-memory-stress
$VALGRIND ./tests/121-multicore-speedup 2
$VALGRIND ./tests/122-multicore-lock-overhead
$VALGRIND ./tests/123-multicore-scalability
