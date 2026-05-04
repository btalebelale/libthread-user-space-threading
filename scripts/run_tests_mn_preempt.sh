#!/bin/bash
# Variante de run_tests_mn.sh : skip 03-equity (incompatible avec préemption).
set -e
export LD_LIBRARY_PATH=.

GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
run_test() {
    echo -e "${CYAN}Lancement du test : $@${NC}"
    "$@"
    echo -e "${GREEN}Succès : $@${NC}\n"
}

echo "===== TESTS MN AVEC PRÉEMPTION (skip 03-equity coop-only) ====="
for t in 01-main 02-switch 11-join 12-join-main 13-join-switch \
         21-create-many 22-create-many-recursive 23-create-many-once \
         31-switch-many 32-switch-many-join 33-switch-many-cascade \
         61-mutex 62-mutex 63-mutex-equity 64-mutex-join 64-sem 65-sem-prodcons \
         81-deadlock 124-signals \
         101-multicore-counter 102-multicore-concurrent-work \
         103-multicore-sem-prodcons 104-multicore-mutex-ordering \
         105-multicore-join-values 106-multicore-cascade-mutex \
         107-multicore-yield-fairness 111-multicore-stress-create-join \
         112-multicore-stress-mutex 113-multicore-memory-stress \
         121-multicore-speedup 122-multicore-lock-overhead 123-multicore-scalability; do
    case "$t" in
        21-create-many|22-create-many-recursive|23-create-many-once) run_test ./tests/$t 200 ;;
        31-switch-many|32-switch-many-join) run_test ./tests/$t 50 50 ;;
        33-switch-many-cascade) run_test ./tests/$t 30 30 ;;
        61-mutex|62-mutex) run_test ./tests/$t 100 ;;
        101-multicore-counter|107-multicore-yield-fairness) run_test ./tests/$t 8 ;;
        121-multicore-speedup) run_test ./tests/$t 4 ;;
        *) run_test ./tests/$t ;;
    esac
done
echo -e "${GREEN}TOUS LES TESTS MN+PREEMPT (sans 03-equity) PASSÉS !${NC}"
