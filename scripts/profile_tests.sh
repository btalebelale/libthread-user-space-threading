#!/bin/bash
# profile_tests.sh — Profil CPU sur la suite de benchmarks.
#
# Usage : ./scripts/profile_tests.sh [--latex]
#   --latex   Génère aussi une table LaTeX prête à copier dans le rapport.
#
# Nécessite : perf (linux-tools-common / linux-tools-$(uname -r))

LATEX=0
for arg in "$@"; do [ "$arg" = "--latex" ] && LATEX=1; done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v perf &>/dev/null; then
    echo "ERREUR : 'perf' n'est pas installe."
    echo "  sudo apt install linux-tools-common linux-tools-\$(uname -r)"
    exit 1
fi

echo "[*] Compilation..."
make -j"$(nproc 2>/dev/null || echo 2)" all >/dev/null 2>&1

export LD_LIBRARY_PATH="$ROOT_DIR"

# Benchmarks représentatifs (assez longs pour échantillonner correctement)
BENCH_CMDS=(
    "./tests/21-create-many 1000"
    "./tests/22-create-many-recursive 500"
    "./tests/23-create-many-once 1000"
    "./tests/31-switch-many 100 100"
    "./tests/32-switch-many-join 100 100"
    "./tests/33-switch-many-cascade 50 50"
    "./tests/51-fibonacci 10"
    "./tests/61-mutex 100"
    "./tests/65-sem-prodcons"
)

# Construire la commande inline en vérifiant les binaires
INLINE=""
for cmd in "${BENCH_CMDS[@]}"; do
    bin=$(echo "$cmd" | awk '{print $1}')
    if [ -x "$bin" ]; then
        INLINE="${INLINE}${cmd} >/dev/null 2>&1; "
    else
        echo "  [SKIP] $bin"
    fi
done

if [ -z "$INLINE" ]; then
    echo "ERREUR : aucun binaire trouvé. Lancez 'make all' d'abord."
    exit 1
fi

PERF_DATA=$(mktemp /tmp/perf_XXXXXX.data)
RESULT_TMP=$(mktemp /tmp/perf_result_XXXXXX.txt)
trap "rm -f '$PERF_DATA' '$RESULT_TMP'" EXIT

echo "[*] Profilage en cours (session unique)..."

# Une seule session perf : les pourcentages sont directement significatifs
perf record -q -F 5000 -o "$PERF_DATA" \
    -- bash -c "export LD_LIBRARY_PATH='$ROOT_DIR'; cd '$ROOT_DIR'; $INLINE" \
    >/dev/null 2>&1 || {
    echo "ERREUR : perf record a echoue. Verifiez les droits perf :"
    echo "  sudo sysctl kernel.perf_event_paranoid=1"
    exit 1
}

echo "[*] Analyse..."

perf report -i "$PERF_DATA" --stdio --no-children -g none \
    --percent-limit 0.01 2>/dev/null | \
awk -v TOP=12 -v THRESH=0.5 '
/^[[:space:]]+[0-9]+\.[0-9]+%/ {
    fn = $NF
    # Filtrer les symboles inutiles
    if (fn ~ /^0x[0-9a-f]+$/) next   # adresses brutes non résolues
    if (fn ~ /^\[/)            next   # modules kernel [kthread], [unknown]
    if (fn == "" || fn == "bash" || fn == "__libc_start_main") next
    pct = $1; gsub(/%/, "", pct); pct += 0
    sums[fn] += pct   # somme : une seule session = pourcentages additifs
}
END {
    if (length(sums) == 0) { print "NO_DATA"; exit }
    n = 0
    for (f in sums) {
        if (sums[f] < THRESH) continue
        n++; fns[n] = f; pcts[n] = sums[f]
    }
    # Tri décroissant
    for (i=1; i<=n; i++)
        for (j=i+1; j<=n; j++)
            if (pcts[j] > pcts[i]) {
                t=pcts[i]; pcts[i]=pcts[j]; pcts[j]=t
                t=fns[i];  fns[i]=fns[j];   fns[j]=t
            }
    if (n > TOP) n = TOP
    for (i=1; i<=n; i++)
        printf "%s\t%.2f\n", fns[i], pcts[i]
}
' > "$RESULT_TMP"

if [ ! -s "$RESULT_TMP" ] || grep -q "^NO_DATA$" "$RESULT_TMP"; then
    echo "Pas de donnees significatives. Essayez :"
    echo "  sudo sysctl kernel.perf_event_paranoid=1"
    exit 1
fi

# ── Affichage texte ────────────────────────────────────────────────────────────
echo ""
echo "================================================================="
echo "   TOP 12 FONCTIONS PAR TEMPS CPU  (seuil >= 0.5%)"
echo "================================================================="
echo ""
printf "%-45s  %11s\n" "Fonction" "% temps CPU"
printf "%-45s  %11s\n" "---------------------------------------------" "-----------"
while IFS=$'\t' read -r fn pct; do
    printf "%-45s  %10s%%\n" "$fn" "$pct"
done < "$RESULT_TMP"

# ── Table LaTeX ────────────────────────────────────────────────────────────────
if [ "$LATEX" = "1" ]; then
    echo ""
    echo "================================================================="
    echo "   TABLE LATEX  (copiez-collez dans votre rapport)"
    echo "================================================================="
    echo ""
    echo '\begin{table}[H]'
    echo '\centering'
    echo '\renewcommand{\arraystretch}{1.3}'
    echo '\begin{tabular}{lr}'
    echo '\toprule'
    echo '\textbf{Fonction} & \textbf{\% temps CPU} \\'
    echo '\midrule'
    while IFS=$'\t' read -r fn pct; do
        fn_tex=$(printf '%s' "$fn" | sed 's/_/\\_/g')
        printf '\\texttt{%s} & %s\\,\\%% \\\\\n' "$fn_tex" "$pct"
    done < "$RESULT_TMP"
    echo '\bottomrule'
    echo '\end{tabular}'
    echo '\caption{Profil d'\''exécution agrégé sur la suite de benchmarks}'
    echo '\label{tab:profile}'
    echo '\end{table}'
fi

echo ""
echo "[*] Termine."
