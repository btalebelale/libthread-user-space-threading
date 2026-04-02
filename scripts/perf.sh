#!/bin/bash
# perf.sh — Profilage et comparaison des implémentations de la bibliothèque de threads
#
# Usage :
#   ./scripts/perf.sh profile                  Affiche les goulots d'étranglement (implémentation courante)
#   ./scripts/perf.sh compare-pools            Compare les 4 implémentations de pool
#   ./scripts/perf.sh compare-all              Compare pools + pthreads
#
# Options :
#   -r N    Nombre de répétitions pour les benchmarks (défaut: 5)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR"

# ── Couleurs ──────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# ── Configuration ─────────────────────────────────────────
POOLS=(tab_pool stailq_pool stailq_pool_prealloc ring_pool)
REPEAT=5
OPT_LEVEL="-Ofast"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Tests intensifs : "source;args;description"
# gprof échantillonne à ~10ms : il faut que chaque test dure > 500ms
# pour obtenir des résultats significatifs
BENCHMARKS_PROFILE=(
    "tests/21-create-many.c;500000;create séquentiel (500k)"
    "tests/22-create-many-recursive.c;50000;create récursif (50k)"
    "tests/23-create-many-once.c;500000;create bulk (500k)"
    "tests/31-switch-many.c;50000 100;yield (50k × 100 th)"
    "tests/33-switch-many-cascade.c;200 200;yield cascade (200 × 200)"
    "tests/51-fibonacci.c;20;fibonacci (fib 20)"
    "tests/bench-ops.c;;bench-ops (unitaire)"
)

# Tests pour les comparaisons de temps (plus légers, répétés N fois)
BENCHMARKS_TIME=(
    "tests/21-create-many;10000;create seq (10k)"
    "tests/22-create-many-recursive;2000;create rec (2k)"
    "tests/23-create-many-once;10000;create bulk (10k)"
    "tests/31-switch-many;1000 50;yield (1k × 50)"
    "tests/32-switch-many-join;50 1000;yield+join (50 × 1k)"
    "tests/33-switch-many-cascade;50 50;cascade (50 × 50)"
    "tests/51-fibonacci;25;fibo (25)"
    "tests/bench-ops;;bench-ops (unitaire)"
)

# ── Fonctions utilitaires ─────────────────────────────────

usage() {
    echo -e "${BOLD}Usage :${NC}"
    echo "  $0 profile              Profiler l'implémentation courante (goulots d'étranglement)"
    echo "  $0 compare-pools        Comparer les implémentations de pool"
    echo "  $0 compare-all          Comparer pools + pthreads"
    echo ""
    echo -e "${BOLD}Options :${NC}"
    echo "  -r N                    Nombre de répétitions pour les benchmarks (défaut: $REPEAT)"
    echo "  -O LEVEL                Niveau d'optimisation : -O0, -O1, -O2, -O3, -Ofast (défaut: $OPT_LEVEL)"
    exit 1
}

# Mesure le temps d'exécution en millisecondes
measure_ms() {
    local start end
    start=$(date +%s%N)
    "$@" > /dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

# Moyenne de N exécutions en ms
bench_avg() {
    local n=$1; shift
    local total=0
    for ((i = 0; i < n; i++)); do
        local t
        t=$(measure_ms "$@")
        total=$((total + t))
    done
    echo $((total / n))
}

# Compile en statique avec -pg pour gprof (profilage)
# Inclut context_switch.S (asm) et wrappe malloc/free pour le profiling
build_static_pg() {
    local pool_impl=$1
    local src=$2
    local out=$3

    local wrapper="$TMP_DIR/alloc_wrappers.c"
    cat > "$wrapper" << 'WRAP_EOF'
#include <stdlib.h>
extern void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) { return __real_malloc(size); }
extern void __real_free(void *);
void __wrap_free(void *ptr) { __real_free(ptr); }
WRAP_EOF

    # ring_pool est header-only (tout inline dans ring_pool.h), pas de .c à compiler
    local pool_src=()
    if [ "$pool_impl" != "ring_pool" ]; then
        pool_src=("src/${pool_impl}.c")
    fi

    gcc -Wall -Wextra -Werror $OPT_LEVEL -pg \
        -I./src -I./debug \
        -o "$out" \
        "$src" src/thread.c src/scheduler.c "${pool_src[@]}" \
        src/context_switch.S "$wrapper" \
        -Wl,--wrap=malloc -Wl,--wrap=free \
        2>&1
}

# Compile avec la bibliothèque partagée (benchmarks de temps)
build_pool() {
    local pool_impl=$1
    make -C "$ROOT_DIR" clean > /dev/null 2>&1
    make -C "$ROOT_DIR" POOL_IMPL="$pool_impl" CFLAGS="-Wall -Wextra -Werror $OPT_LEVEL -fPIC -I./src -I./debug" all > /dev/null 2>&1
}

build_pthreads() {
    make -C "$ROOT_DIR" CFLAGS="-Wall -Wextra -Werror $OPT_LEVEL -fPIC -I./src -I./debug" pthreads > /dev/null 2>&1
}

separator() {
    echo -e "${CYAN}────────────────────────────────────────────────────────────────${NC}"
}

# ── Mode : profile ────────────────────────────────────────

do_profile() {
    local pool_impl="${1:-stailq_pool}"

    echo -e "${BOLD}${YELLOW}▶ Profiling avec gprof (pool: $pool_impl, opt: $OPT_LEVEL)${NC}"
    echo -e "${DIM}  Compilation statique avec -pg pour capturer les appels système${NC}"
    separator
    echo ""

    for bench in "${BENCHMARKS_PROFILE[@]}"; do
        IFS=';' read -r src args desc <<< "$bench"
        local bin="$TMP_DIR/bench_bin"

        echo -e "${BOLD}── $desc ──${NC}"
        echo -e "  ${DIM}Source : $src | Args : $args${NC}"

        # Compilation statique avec -pg
        if ! build_static_pg "$pool_impl" "$src" "$bin" > /dev/null 2>&1; then
            echo -e "  ${RED}Erreur de compilation${NC}"
            echo ""
            continue
        fi

        # Exécution
        (cd "$TMP_DIR" && "$bin" $args > /dev/null 2>&1) || true

        if [ ! -f "$TMP_DIR/gmon.out" ]; then
            echo -e "  ${RED}gmon.out non généré${NC}"
            echo ""
            continue
        fi

        # Analyse gprof — flat profile trié par temps
        echo ""
        echo -e "  ${YELLOW}Fonctions triées par temps (flat profile) :${NC}"
        echo ""

        # En-tête : gprof colonnes = %time, cumul, self, calls, self/call, total/call, name
        printf "  ${BOLD}%7s  %8s  %10s  %-30s${NC}\n" "% temps" "self (s)" "appels" "fonction"
        printf "  %7s  %8s  %10s  %-30s\n" "───────" "────────" "──────────" "──────────────────────────────"

        gprof "$bin" "$TMP_DIR/gmon.out" --flat-profile --brief 2>/dev/null \
        | awk 'NR>=6 && NF>=7 && $1 ~ /^[0-9]/ {printf "%7s  %8s  %10s  %s\n", $1, $3, $4, $7}' \
        | head -20 \
        | while IFS= read -r line; do
            local name
            name=$(echo "$line" | awk '{print $4}')

            [ -z "$name" ] && continue

            # Colorer selon la catégorie
            local color="$NC"
            local tag=""
            if echo "$name" | grep -qE '^(pool_|is_pool)'; then
                color="$YELLOW"; tag="pool"
            elif echo "$name" | grep -qE '^(sched_|is_sched)'; then
                color="$YELLOW"; tag="scheduler"
            elif echo "$name" | grep -qE '^thread_'; then
                color="$CYAN"; tag="thread API"
            elif echo "$name" | grep -qE '^(__wrap_malloc|__wrap_free|malloc|free|calloc|realloc|cfree|aligned_alloc)'; then
                color="$RED"; tag="allocateur"
            elif echo "$name" | grep -qE '^(context_switch|context_restore|thread_trampoline|thread_entry)'; then
                color="$GREEN"; tag="context switch"
            fi

            if [ -n "$tag" ]; then
                printf "  ${color}%s ← %s${NC}\n" "$line" "$tag"
            else
                printf "  %s\n" "$line"
            fi
        done

        echo ""
        rm -f "$TMP_DIR/gmon.out"
    done

    separator
    echo -e "${BOLD}Légende :${NC}"
    echo -e "  ${RED}rouge${NC}   = allocateur (malloc/free)"
    echo -e "  ${GREEN}vert${NC}    = context switch (asm)"
    echo -e "  ${YELLOW}jaune${NC}   = pool / scheduler"
    echo -e "  ${CYAN}cyan${NC}    = API thread (thread_yield, thread_create, ...)"
    echo ""
}

# ── Mode : compare-pools ─────────────────────────────────

do_compare_pools() {
    echo -e "${BOLD}${YELLOW}▶ Comparaison des implémentations de pool (opt: $OPT_LEVEL)${NC}"
    echo -e "  Répétitions par test : ${BOLD}$REPEAT${NC}"
    separator
    echo ""

    # En-tête du tableau
    printf "${BOLD}%-25s" "Pool impl"
    for bench in "${BENCHMARKS_TIME[@]}"; do
        IFS=';' read -r _ _ desc <<< "$bench"
        printf "│ %-24s" "$desc"
    done
    printf "${NC}\n"

    printf "%-25s" "─────────────────────────"
    for _ in "${BENCHMARKS_TIME[@]}"; do
        printf "┼─────────────────────────"
    done
    printf "\n"

    # Stocker les résultats pour le résumé
    declare -A results

    for pool in "${POOLS[@]}"; do
        echo -ne "\033[2K  ${DIM}Compilation $pool...${NC}\r" >&2
        build_pool "$pool"

        printf "%-25s" "$pool"

        for bench in "${BENCHMARKS_TIME[@]}"; do
            IFS=';' read -r bin args desc <<< "$bench"
            echo -ne "\033[2K  ${DIM}$pool : $desc...${NC}\r" >&2
            local avg
            avg=$(bench_avg "$REPEAT" ./$bin $args)
            results["${pool}:${desc}"]=$avg
            printf "│ %18s ms    " "$avg"
        done
        echo -ne "\033[2K\r" >&2
        printf "\n"
    done
    echo ""
    separator

    # Meilleur par test
    echo -e "${BOLD}Meilleure implémentation par test :${NC}"
    for bench in "${BENCHMARKS_TIME[@]}"; do
        IFS=';' read -r _ _ desc <<< "$bench"
        local best_pool="" best_time=999999999
        for pool in "${POOLS[@]}"; do
            local t=${results["${pool}:${desc}"]}
            if (( t < best_time )); then
                best_time=$t
                best_pool=$pool
            fi
        done
        echo -e "  ${GREEN}$desc${NC} → ${BOLD}$best_pool${NC} (${best_time} ms)"
    done
    echo ""
}

# ── Mode : compare-all ───────────────────────────────────

do_compare_all() {
    echo -e "${BOLD}${YELLOW}▶ Comparaison complète : pools + pthreads (opt: $OPT_LEVEL)${NC}"
    echo -e "  Répétitions par test : ${BOLD}$REPEAT${NC}"
    separator
    echo ""

    # En-tête
    printf "${BOLD}%-25s" "Implémentation"
    for bench in "${BENCHMARKS_TIME[@]}"; do
        IFS=';' read -r _ _ desc <<< "$bench"
        printf "│ %-24s" "$desc"
    done
    printf "${NC}\n"

    printf "%-25s" "─────────────────────────"
    for _ in "${BENCHMARKS_TIME[@]}"; do
        printf "┼─────────────────────────"
    done
    printf "\n"

    declare -A results

    # Pools
    for pool in "${POOLS[@]}"; do
        echo -ne "\033[2K  ${DIM}Compilation $pool...${NC}\r" >&2
        build_pool "$pool"

        printf "%-25s" "$pool"
        for bench in "${BENCHMARKS_TIME[@]}"; do
            IFS=';' read -r bin args desc <<< "$bench"
            echo -ne "\033[2K  ${DIM}$pool : $desc...${NC}\r" >&2
            local avg
            avg=$(bench_avg "$REPEAT" ./$bin $args)
            results["${pool}:${desc}"]=$avg
            printf "│ %18s ms    " "$avg"
        done
        echo -ne "\033[2K\r" >&2
        printf "\n"
    done

    # Pthreads
    echo -ne "\033[2K  ${DIM}Compilation pthreads...${NC}\r" >&2
    build_pool "stailq_pool"
    build_pthreads

    printf "%-25s" "pthread (baseline)"
    for bench in "${BENCHMARKS_TIME[@]}"; do
        IFS=';' read -r bin args desc <<< "$bench"
        local pthread_bin="${bin}-pthread"
        if [ -f "$pthread_bin" ]; then
            echo -ne "\033[2K  ${DIM}pthread : $desc...${NC}\r" >&2
            local avg
            avg=$(bench_avg "$REPEAT" ./$pthread_bin $args)
            results["pthread:${desc}"]=$avg
            printf "│ %18s ms    " "$avg"
        else
            printf "│ %24s" "N/A"
        fi
    done
    echo -ne "\033[2K\r" >&2
    printf "\n"
    echo ""
    separator

    # Meilleur par test
    local all_impls=("${POOLS[@]}" "pthread")
    echo -e "${BOLD}Meilleure implémentation par test :${NC}"
    for bench in "${BENCHMARKS_TIME[@]}"; do
        IFS=';' read -r _ _ desc <<< "$bench"
        local best_impl="" best_time=999999999
        for impl in "${all_impls[@]}"; do
            local t=${results["${impl}:${desc}"]:-999999999}
            if (( t < best_time && t > 0 )); then
                best_time=$t
                best_impl=$impl
            fi
        done
        echo -e "  ${GREEN}$desc${NC} → ${BOLD}$best_impl${NC} (${best_time} ms)"
    done
    echo ""
}

# ── Parsing des arguments ─────────────────────────────────

cmd=""
pool_override=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        profile)        cmd="profile"; shift ;;
        compare-pools)  cmd="compare-pools"; shift ;;
        compare-all)    cmd="compare-all"; shift ;;
        -r)             REPEAT="$2"; shift 2 ;;
        -p)             pool_override="$2"; shift 2 ;;
        -O)             OPT_LEVEL="-O$2"; shift 2 ;;
        -h|--help)      usage ;;
        *)              echo -e "${RED}Commande inconnue : $1${NC}"; usage ;;
    esac
done

if [[ -z "$cmd" ]]; then
    usage
fi

case "$cmd" in
    profile)        do_profile "${pool_override:-ring_pool}" ;;
    compare-pools)  do_compare_pools ;;
    compare-all)    do_compare_all ;;
esac
