#!/bin/bash
# profile_tests.sh — Lance les tests existants et affiche le % de temps
# passé dans chaque fonction (fonctions internes + appels système).
#
# Usage : ./scripts/profile_tests.sh
#
# Nécessite : perf (linux-tools-common / linux-tools-$(uname -r))
#
# Portable : fonctionne quelle que soit l'implémentation, tant que
# le Makefile produit les binaires de tests et libthread.so.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

# Vérifier que perf est disponible
if ! command -v perf &>/dev/null; then
    echo "ERREUR : 'perf' n'est pas installe."
    echo "  sudo apt install linux-tools-common linux-tools-\$(uname -r)"
    exit 1
fi

# 1. Compiler tout (sans LTO ni -Ofast pour garder les symboles lisibles)
echo "[*] Compilation de la bibliotheque et des tests..."
make -j"$(nproc 2>/dev/null || echo 2)" all

export LD_LIBRARY_PATH=.

# Liste des tests avec leurs arguments (même chose que run_tests.sh)
TESTS=(
    "./tests/01-main"
    "./tests/02-switch"
    "./tests/03-equity"
    "./tests/11-join"
    "./tests/12-join-main"
    "./tests/13-join-switch"
    "./tests/21-create-many 1000"
    "./tests/22-create-many-recursive 500"
    "./tests/23-create-many-once 1000"
    "./tests/31-switch-many 100 100"
    "./tests/32-switch-many-join 100 100"
    "./tests/33-switch-many-cascade 50 50"
    "./tests/51-fibonacci 10"
    "./tests/61-mutex 100"
    "./tests/62-mutex 100"
    "./tests/63-mutex-equity"
    "./tests/64-mutex-join"
    "./tests/64-sem"
    "./tests/65-sem-prodcons"
    "./tests/71-preemption 100"
    "./tests/81-deadlock"
)

TMPDIR_PROF=$(mktemp -d /tmp/perf_profile_XXXXXX)

cleanup() {
    rm -rf "$TMPDIR_PROF"
}
trap cleanup EXIT

echo "[*] Profilage de chaque test avec perf..."

ALL_PERF_DATA=()
for test_cmd in "${TESTS[@]}"; do
    test_bin=$(echo "$test_cmd" | awk '{print $1}')
    if [ ! -x "$test_bin" ]; then
        echo "  [SKIP] $test_bin (binaire introuvable)"
        continue
    fi

    data_file="$TMPDIR_PROF/$(basename "$test_bin").data"
    ALL_PERF_DATA+=("$data_file")

    echo "  [RUN] $test_cmd"
    # -F 5000 : fréquence d'échantillonnage élevée (les tests sont courts)
    # -g --call-graph dwarf : call stacks précis
    perf record -q -F 5000 -g --call-graph dwarf -o "$data_file" -- $test_cmd >/dev/null 2>&1 || {
        perf record -q -F 5000 -g -o "$data_file" -- $test_cmd >/dev/null 2>&1 || {
            echo "  [FAIL] perf record a echoue pour: $test_cmd"
            rm -f "$data_file"
            ALL_PERF_DATA=("${ALL_PERF_DATA[@]/$data_file}")
            continue
        }
    }
done

COMBINED="$TMPDIR_PROF/combined.txt"
> "$COMBINED"

for data_file in "${ALL_PERF_DATA[@]}"; do
    [ -z "$data_file" ] && continue
    [ ! -f "$data_file" ] && continue
    perf report -i "$data_file" --stdio --no-children -g none \
        --percent-limit 0.01 2>/dev/null >> "$COMBINED" || true
done

echo ""
echo "================================================================="
echo "   PROFIL AGREGE — % de temps par fonction"
echo "================================================================="
echo ""

# Extraire et agréger les résultats, en filtrant les adresses kernel brutes
awk '
/^[[:space:]]+[0-9]+\.[0-9]+%/ {
    line = $0
    # Supprimer les espaces de tête
    gsub(/^[[:space:]]+/, "", line)
    # Extraire le pourcentage
    pct = line
    sub(/%.*/, "", pct)
    pct += 0
    # Extraire le nom de fonction (dernier champ)
    fn = $NF
    # Filtrer les adresses kernel brutes (0xffffffff...)
    if (fn ~ /^0x[0-9a-f]+$/) next
    sums[fn] += pct
}
END {
    total = 0
    for (f in sums) total += sums[f]
    if (total == 0) { print "Pas de donnees de profilage."; exit }
    # Collecter dans un tableau indexé
    n = 0
    for (f in sums) { n++; fns[n] = f; pcts[n] = sums[f] / total * 100 }
    # Tri décroissant
    for (i = 1; i <= n; i++)
        for (j = i + 1; j <= n; j++)
            if (pcts[j] > pcts[i]) {
                tmp = pcts[i]; pcts[i] = pcts[j]; pcts[j] = tmp
                tmp = fns[i]; fns[i] = fns[j]; fns[j] = tmp
            }
    printf "%-45s  %8s\n", "Fonction", "% temps"
    printf "%-45s  %8s\n", "---------------------------------------------", "--------"
    for (i = 1; i <= n; i++) {
        if (pcts[i] < 0.10) continue
        printf "%-45s  %7.2f%%\n", fns[i], pcts[i]
    }
}
' "$COMBINED"

echo ""
echo "================================================================="
echo "   DETAIL PAR TEST"
echo "================================================================="

for data_file in "${ALL_PERF_DATA[@]}"; do
    [ -z "$data_file" ] && continue
    [ ! -f "$data_file" ] && continue
    test_name=$(basename "$data_file" .data)
    echo ""
    echo "--- $test_name ---"
    perf report -i "$data_file" --stdio --no-children -g none \
        --percent-limit 1.0 2>/dev/null | \
        awk '/^[[:space:]]+[0-9]+\.[0-9]+%/ {
            fn = $NF
            if (fn ~ /^0x[0-9a-f]+$/) next
            gsub(/^[[:space:]]+/, "")
            print
        }' || true
done

echo ""
echo "[*] Profilage termine."
