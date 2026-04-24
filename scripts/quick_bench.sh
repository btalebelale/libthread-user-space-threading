#!/bin/bash
set -euo pipefail
export LC_ALL=C
export LD_LIBRARY_PATH=.

RUNS=11

median() {
    sort -n | awk '{a[NR]=$1} END {
        if (NR%2==1) print a[(NR+1)/2];
        else printf "%.0f\n", (a[NR/2]+a[NR/2+1])/2
    }'
}

extract_us() {
    local test_name="$1" line="$2"
    if [[ "$test_name" == "51-fibonacci" ]]; then
        echo "$line" | awk -F';' '{printf "%.0f\n", $(NF-1) * 1000000}'
    else
        echo "$line" | awk -F';' '{print $NF}'
    fi
}

declare -A ARGS=(
    [21-create-many]="50000"
    [22-create-many-recursive]="5000"
    [23-create-many-once]="50000"
    [31-switch-many]="500 500"
    [32-switch-many-join]="500 500"
    [33-switch-many-cascade]="200 200"
    [51-fibonacci]="24"
)

TESTS=(21-create-many 22-create-many-recursive 23-create-many-once
       31-switch-many 32-switch-many-join 33-switch-many-cascade
       51-fibonacci)

printf "%-30s %s\n" "TEST" "MEDIAN_us"
for t in "${TESTS[@]}"; do
    args="${ARGS[$t]}"
    times=()
    for ((r=0; r<RUNS; r++)); do
        gline=$(./tests/$t $args 2>&1 | grep '^GRAPH' | tail -1)
        times+=( "$(extract_us "$t" "$gline")" )
    done
    med=$(printf '%s\n' "${times[@]}" | median)
    printf "%-30s %s\n" "$t" "$med"
done
