#!/bin/bash
set -e

VALGRIND="valgrind --leak-check=full --show-reachable=yes --track-origins=yes"

echo "===== BASIC ====="
$VALGRIND ./tests/01-main
$VALGRIND ./tests/02-switch
$VALGRIND ./tests/03-equity

echo "===== JOIN ====="
$VALGRIND ./tests/11-join
$VALGRIND ./tests/12-join-main
$VALGRIND ./tests/13-join-switch

echo "===== CREATE ====="
$VALGRIND ./tests/21-create-many 100
$VALGRIND ./tests/22-create-many-recursive 200
$VALGRIND ./tests/23-create-many-once 100

echo "===== SWITCH ====="
$VALGRIND ./tests/31-switch-many 20 20
$VALGRIND ./tests/32-switch-many-join 20 20
$VALGRIND ./tests/33-switch-many-cascade 10 10

echo "===== FIBO ====="
$VALGRIND ./tests/51-fibonacci 10