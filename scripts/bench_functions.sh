#!/bin/bash
# bench_functions.sh — Compile et lance le micro-benchmark de chaque fonction.
#
# Usage : ./scripts/bench_functions.sh
#
# Portable : fonctionne quelle que soit l'implémentation, tant que
# le Makefile produit libthread.so et que src/thread.h existe.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

# 1. Compiler la bibliothèque
echo "[*] Compilation de la bibliotheque..."
make -j"$(nproc 2>/dev/null || echo 2)" all

# 2. Compiler le benchmark
echo "[*] Compilation du benchmark..."
gcc -O2 -Wall -Wextra -I./src scripts/bench_functions.c -o bench_functions -L. -lthread -lrt 2>/dev/null \
  || gcc -O2 -Wall -Wextra -I./src scripts/bench_functions.c -o bench_functions -L. -lthread

# 3. Lancer
echo "[*] Lancement du benchmark..."
echo ""
LD_LIBRARY_PATH=. ./bench_functions

# 4. Nettoyage
rm -f bench_functions
