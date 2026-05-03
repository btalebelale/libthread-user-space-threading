#!/bin/bash
set -e

export LD_LIBRARY_PATH=.

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

run_test() {
    echo -e "${CYAN}Lancement du test : $@${NC}"
    "$@"
    echo -e "${GREEN}Succès : $@${NC}\n"
}

echo "===== TESTS FONCTIONNELS DE BASE (en mode M:N) ====="
run_test ./tests/01-main
run_test ./tests/02-switch
run_test ./tests/03-equity
run_test ./tests/11-join
run_test ./tests/12-join-main
run_test ./tests/13-join-switch
run_test ./tests/21-create-many 200
run_test ./tests/22-create-many-recursive 200
run_test ./tests/23-create-many-once 200
run_test ./tests/31-switch-many 50 50
run_test ./tests/32-switch-many-join 50 50
run_test ./tests/33-switch-many-cascade 30 30
run_test ./tests/61-mutex 100
run_test ./tests/62-mutex 100
run_test ./tests/63-mutex-equity
run_test ./tests/64-mutex-join
run_test ./tests/64-sem
run_test ./tests/65-sem-prodcons
run_test ./tests/81-deadlock
run_test ./tests/124-signals

echo "===== TESTS MULTICORE ====="
run_test ./tests/101-multicore-counter 8
run_test ./tests/102-multicore-concurrent-work
run_test ./tests/103-multicore-sem-prodcons
run_test ./tests/104-multicore-mutex-ordering
run_test ./tests/105-multicore-join-values
run_test ./tests/106-multicore-cascade-mutex
run_test ./tests/107-multicore-yield-fairness 8
run_test ./tests/111-multicore-stress-create-join
run_test ./tests/112-multicore-stress-mutex
run_test ./tests/113-multicore-memory-stress
run_test ./tests/121-multicore-speedup 4
run_test ./tests/122-multicore-lock-overhead
run_test ./tests/123-multicore-scalability

echo -e "${GREEN}==========================================${NC}"
echo -e "${GREEN}TOUS LES TESTS M:N SONT PASSÉS !${NC}"
echo -e "${GREEN}==========================================${NC}"
