#!/bin/bash
set -e

export LD_LIBRARY_PATH=.

GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m' 

run_test() {
    echo -e "${CYAN}Lancement du test : $@${NC}"
    "$@"
    echo -e "${GREEN}Succès : $@${NC}\n"
}

echo "===== BASIC TESTS ========="
run_test ./tests/01-main
run_test ./tests/02-switch
run_test ./tests/03-equity

echo "===== JOIN TESTS =========="
run_test ./tests/11-join
run_test ./tests/12-join-main
run_test ./tests/13-join-switch

echo "===== CREATE TESTS ========"
run_test ./tests/21-create-many 1000
run_test ./tests/22-create-many-recursive 500
run_test ./tests/23-create-many-once 1000

echo "===== SWITCH TESTS ========"
run_test ./tests/31-switch-many 100 100
run_test ./tests/32-switch-many-join 100 100
run_test ./tests/33-switch-many-cascade 50 50

echo "===== FIBO ================"
run_test ./tests/51-fibonacci 10

echo "===== MUTEX ==============="
run_test ./tests/61-mutex 100
run_test ./tests/62-mutex 100
run_test ./tests/63-mutex-equity
run_test ./tests/64-mutex-join

#echo "===== PREEMPTION =========="
#run_test ./tests/71-preemption 100

#echo "===== DEADLOCK ============"
#run_test ./tests/81-deadlock

echo -e "${GREEN}==========================================${NC}"
echo -e "${GREEN}TOUS LES TESTS SONT PASSÉS AVEC SUCCÈS !${NC}"
echo -e "${GREEN}==========================================${NC}"
