#!/bin/bash
set -e
cd "$(dirname "$0")/.."

RING_BITS=${1:-17}

echo ">>> Build SoA (current)..."
make clean -s
make RING_BITS=$RING_BITS -s
gcc -Ofast -flto -march=native -mtune=native -I./src bench/bench_dedup_table.c -o bench/bench_soa -L. -lthread
cp libthread.so libthread_soa.so

echo ">>> Build Swiss Table..."
make clean -s
make RING_BITS=$RING_BITS EXTRA_CFLAGS="-DDEDUP_SWISS" -s
gcc -Ofast -flto -march=native -mtune=native -DDEDUP_SWISS -I./src bench/bench_dedup_table.c -o bench/bench_swiss -L. -lthread
cp libthread.so libthread_swiss.so

echo ">>> Run SoA..."
cp libthread_soa.so libthread.so
LD_LIBRARY_PATH=. ./bench/bench_soa 2>bench_soa.csv

echo ">>> Run Swiss..."
cp libthread_swiss.so libthread.so
LD_LIBRARY_PATH=. ./bench/bench_swiss 2>bench_swiss.csv

echo ""
paste -d',' bench_soa.csv bench_swiss.csv | awk -F',' '
BEGIN {
    printf "┌─────────────────────────────────┬────────────┬────────────┬──────────┐\n"
    printf "│ %-31s │ %10s │ %10s │ %8s │\n", "Bench", "SoA", "Swiss", "Winner"
    printf "├─────────────────────────────────┼────────────┼────────────┼──────────┤\n"
}
{
    label = $2
    n     = $3
    soa   = $4 + 0
    swiss = $8 + 0

    if (label == "dup_hit")
        desc = "Dup hit (100k lookups)"
    else if (label == "cycle")
        desc = "Cycle create+join (10k)"
    else
        desc = sprintf("Miss (%6d active)", n)

    if (soa > 0 && swiss > 0) {
        if (soa <= swiss) {
            ratio = swiss / soa
            arrow = sprintf("%.2fx SoA", ratio)
        } else {
            ratio = soa / swiss
            arrow = sprintf("%.2fx Swi", ratio)
        }
    } else {
        arrow = "N/A"
    }

    fmt_s = (soa >= 1000000) ? sprintf("%7.2f ms", soa/1e6) : (soa >= 1000) ? sprintf("%7.1f µs", soa/1000) : sprintf("%7.1f ns", soa)
    fmt_w = (swiss >= 1000000) ? sprintf("%7.2f ms", swiss/1e6) : (swiss >= 1000) ? sprintf("%7.1f µs", swiss/1000) : sprintf("%7.1f ns", swiss)

    printf "│ %-31s │ %10s │ %10s │ %8s │\n", desc, fmt_s, fmt_w, arrow
}
END {
    printf "└─────────────────────────────────┴────────────┴────────────┴──────────┘\n"
}'

# Cleanup
rm -f libthread_soa.so libthread_swiss.so bench/bench_soa bench/bench_swiss
make clean -s && make -s
