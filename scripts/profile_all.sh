#!/bin/bash
set -euo pipefail
export LC_ALL=C

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULT_DIR="$PROJ_DIR/profile_results"
RUNS=11
PERF_RUNS=5

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

mkdir -p "$RESULT_DIR"
cd "$PROJ_DIR"
export LD_LIBRARY_PATH=.

TESTS=(21-create-many 22-create-many-recursive 23-create-many-once
       31-switch-many 32-switch-many-join 33-switch-many-cascade
       51-fibonacci)

declare -A STD_ARGS=(
    [21-create-many]="1000"
    [22-create-many-recursive]="500"
    [23-create-many-once]="1000"
    [31-switch-many]="100 100"
    [32-switch-many-join]="100 100"
    [33-switch-many-cascade]="50 50"
    [51-fibonacci]="10"
)

declare -A PROF_ARGS=(
    [21-create-many]="50000"
    [22-create-many-recursive]="5000"
    [23-create-many-once]="50000"
    [31-switch-many]="1000 1000"
    [32-switch-many-join]="1000 1000"
    [33-switch-many-cascade]="500 500"
    [51-fibonacci]="24"
)

extract_us() {
    local test_name="$1"
    local line="$2"
    if [[ "$test_name" == "51-fibonacci" ]]; then
        echo "$line" | awk -F';' '{printf "%.0f\n", $(NF-1) * 1000000}'
    else
        echo "$line" | awk -F';' '{print $NF}'
    fi
}

median() {
    sort -n | awk '{a[NR]=$1} END {
        if (NR%2==1) print a[(NR+1)/2];
        else printf "%.0f\n", (a[NR/2]+a[NR/2+1])/2
    }'
}

# ===========================================================
# NIVEAU 1 : Comparaison macro libthread vs pthread
# ===========================================================
phase1_macro() {
    echo -e "\n${BOLD}${CYAN}=== NIVEAU 1 : Comparaison libthread vs pthread ===${NC}\n"

    local outfile="$RESULT_DIR/macro_comparison.tsv"
    printf "%-30s %-15s %-15s %-15s %-10s %s\n" \
        "TEST" "ARGS" "LIBTHREAD_us" "PTHREAD_us" "RATIO" "VERDICT" > "$outfile"

    for t in "${TESTS[@]}"; do
        local args="${STD_ARGS[$t]}"
        local lib_times=()
        local pthr_times=()

        echo -e "${CYAN}Benchmarking $t ($args) x$RUNS...${NC}"

        for ((r=0; r<RUNS; r++)); do
            local gline
            gline=$(./tests/$t $args 2>&1 | grep '^GRAPH' | tail -1)
            lib_times+=( "$(extract_us "$t" "$gline")" )
        done

        if [[ -x "./tests/${t}-pthread" ]]; then
            for ((r=0; r<RUNS; r++)); do
                local gline
                gline=$(./tests/${t}-pthread $args 2>&1 | grep '^GRAPH' | tail -1)
                pthr_times+=( "$(extract_us "$t" "$gline")" )
            done
        fi

        local lib_med pthr_med
        lib_med=$(printf '%s\n' "${lib_times[@]}" | median)

        if [[ ${#pthr_times[@]} -gt 0 ]]; then
            pthr_med=$(printf '%s\n' "${pthr_times[@]}" | median)
        else
            pthr_med="N/A"
        fi

        local ratio verdict
        if [[ "$pthr_med" != "N/A" && "$pthr_med" -gt 0 ]]; then
            ratio=$(awk "BEGIN {printf \"%.2f\", $lib_med / $pthr_med}")
            if (( $(awk "BEGIN {print ($ratio > 1.5) ? 1 : 0}") )); then
                verdict="BOTTLENECK"
            elif (( $(awk "BEGIN {print ($ratio > 1.0) ? 1 : 0}") )); then
                verdict="SLOWER"
            else
                verdict="FASTER"
            fi
        else
            ratio="N/A"
            verdict="NO_PTHREAD"
        fi

        local color="$GREEN"
        [[ "$verdict" == "SLOWER" ]] && color="$YELLOW"
        [[ "$verdict" == "BOTTLENECK" ]] && color="$RED"

        printf "%-30s %-15s %-15s %-15s %-10s %s\n" \
            "$t" "$args" "$lib_med" "$pthr_med" "${ratio}x" "$verdict" >> "$outfile"
        echo -e "  ${color}${t}: ${lib_med}us vs ${pthr_med}us = ${ratio}x [${verdict}]${NC}"
    done

    echo -e "\n${BOLD}Resultats: $outfile${NC}"
    cat "$outfile"
}

# ===========================================================
# NIVEAU 2 : Profiling par fonction (perf record)
# ===========================================================
phase2_perf_record() {
    echo -e "\n${BOLD}${CYAN}=== NIVEAU 2 : Profiling par fonction (perf record) ===${NC}\n"

    if ! command -v perf &>/dev/null; then
        echo -e "${RED}perf non installe, niveau 2 ignore${NC}"
        return
    fi

    for t in "${TESTS[@]}"; do
        local args="${PROF_ARGS[$t]}"
        local datafile="$RESULT_DIR/perf_${t}.data"
        local reportfile="$RESULT_DIR/perf_${t}_report.txt"

        echo -e "${CYAN}perf record: $t ($args)...${NC}"
        perf record -e cycles:u -F 32000 -o "$datafile" \
            -- ./tests/$t $args >/dev/null 2>&1 || true

        perf report -i "$datafile" --stdio --no-children \
            --percent-limit=0.5 > "$reportfile" 2>/dev/null || true

        echo -e "  ${GREEN}-> $reportfile${NC}"
        head -30 "$reportfile" | grep -E '^\s+[0-9]' | head -10 || true
        echo ""
    done
}

# ===========================================================
# NIVEAU 3 : Compteurs hardware (perf stat)
# ===========================================================
phase3_hw_counters() {
    echo -e "\n${BOLD}${CYAN}=== NIVEAU 3 : Compteurs hardware (perf stat) ===${NC}\n"

    if ! command -v perf &>/dev/null; then
        echo -e "${RED}perf non installe, niveau 3 ignore${NC}"
        return
    fi

    local outfile="$RESULT_DIR/hwcounters.tsv"
    printf "%-30s %-8s %-12s %-12s %s\n" \
        "TEST" "IPC" "CACHE_MISS%" "BRANCH_MISS%" "DIAGNOSIS" > "$outfile"

    local events="cycles,instructions,cache-references,cache-misses,branches,branch-misses"

    for t in "${TESTS[@]}"; do
        local args="${PROF_ARGS[$t]}"
        local statfile="$RESULT_DIR/perf_stat_${t}.txt"

        echo -e "${CYAN}perf stat: $t ($args) x$PERF_RUNS...${NC}"
        perf stat -e "$events" -r "$PERF_RUNS" \
            -- ./tests/$t $args > /dev/null 2> "$statfile" || true

        # Parse perf stat output: extract number before each event name
        parse_counter() {
            local event="$1" file="$2"
            local val
            val=$(grep "$event" "$file" | grep -v 'not supported\|not counted' | head -1 \
                | sed 's/^[[:space:]]*//' | awk '{gsub(/,/,"",$1); print $1}')
            echo "${val:-0}"
        }

        local cycles instr cache_ref cache_miss branches br_miss
        cycles=$(parse_counter 'cycles' "$statfile")
        instr=$(parse_counter 'instructions' "$statfile")
        cache_ref=$(parse_counter 'cache-references' "$statfile")
        cache_miss=$(parse_counter 'cache-misses' "$statfile")
        branches=$(parse_counter 'branches' "$statfile")
        br_miss=$(parse_counter 'branch-misses' "$statfile")

        # Compute derived metrics
        local ipc cache_pct br_pct diagnosis
        ipc=$(awk "BEGIN {c=$cycles+0; if (c>0) printf \"%.2f\", ($instr+0)/c; else print \"N/A\"}")
        cache_pct=$(awk "BEGIN {r=$cache_ref+0; if (r>0) printf \"%.1f\", ($cache_miss+0)/r*100; else print \"0.0\"}")
        br_pct=$(awk "BEGIN {b=$branches+0; if (b>0) printf \"%.1f\", ($br_miss+0)/b*100; else print \"0.0\"}")

        # Diagnosis
        diagnosis="OK"
        if awk "BEGIN {exit !($ipc+0 < 1.0)}"; then
            if awk "BEGIN {exit !($cache_pct+0 > 5.0)}"; then
                diagnosis="CACHE-BOUND"
            elif awk "BEGIN {exit !($br_pct+0 > 2.0)}"; then
                diagnosis="BRANCH-BOUND"
            else
                diagnosis="STALLED"
            fi
        elif awk "BEGIN {exit !($ipc+0 >= 1.5)}"; then
            diagnosis="CPU-BOUND"
        fi

        local color="$GREEN"
        [[ "$diagnosis" != "OK" && "$diagnosis" != "CPU-BOUND" ]] && color="$RED"

        printf "%-30s %-8s %-12s %-12s %s\n" \
            "$t" "$ipc" "${cache_pct}%" "${br_pct}%" "$diagnosis" >> "$outfile"
        echo -e "  ${color}${t}: IPC=${ipc} cache=${cache_pct}% branch=${br_pct}% [${diagnosis}]${NC}"
    done

    echo -e "\n${BOLD}Resultats: $outfile${NC}"
    cat "$outfile"
}

# ===========================================================
# BONUS : Objdump des fonctions chaudes
# ===========================================================
phase4_objdump() {
    echo -e "\n${BOLD}${CYAN}=== BONUS : Desassemblage des fonctions chaudes ===${NC}\n"

    local outfile="$RESULT_DIR/hot_functions.asm"
    local funcs=(thread_yield thread_create thread_join thread_exit context_switch context_restore)

    > "$outfile"
    local full_dump
    full_dump=$(objdump -d -M intel --no-show-raw-insn libthread.so)

    for fn in "${funcs[@]}"; do
        echo "=== $fn ===" >> "$outfile"
        echo "$full_dump" | awk "/^[0-9a-f]+ <${fn}>:/,/^$/" >> "$outfile"
        echo "" >> "$outfile"
    done

    local nlines
    nlines=$(wc -l < "$outfile")
    echo -e "${GREEN}$outfile ($nlines lignes)${NC}"
    echo -e "Fonctions extraites: ${funcs[*]}"
}

# ===========================================================
# RESUME
# ===========================================================
summary() {
    echo -e "\n${BOLD}${CYAN}=========================================${NC}"
    echo -e "${BOLD}${CYAN}  RESUME DES RESULTATS${NC}"
    echo -e "${BOLD}${CYAN}=========================================${NC}\n"

    echo -e "${BOLD}Fichiers generes dans $RESULT_DIR/ :${NC}"
    ls -1 "$RESULT_DIR/" | sed 's/^/  /'

    echo -e "\n${BOLD}Prochaines etapes :${NC}"
    echo "  1. cat profile_results/macro_comparison.tsv   # Quels tests sont lents ?"
    echo "  2. cat profile_results/hwcounters.tsv          # Pourquoi ?"
    echo "  3. cat profile_results/perf_XX_report.txt      # Ou exactement ?"
    echo "  4. cat profile_results/hot_functions.asm        # Code genere correct ?"
}

# ===========================================================
# MAIN
# ===========================================================
echo -e "${BOLD}${CYAN}Profilage des tests 21-51${NC}"
echo -e "Resultats: $RESULT_DIR/\n"

phase1_macro
phase2_perf_record
phase3_hw_counters
phase4_objdump
summary
