#!/bin/bash
# profile_tests.sh — Profil CPU par test, pour identifier les bottlenecks.
#
# Usage : ./scripts/profile_tests.sh [--latex]
#   --latex   Génère aussi une table LaTeX par test.
#
# Pour chaque benchmark, on lance une session perf isolée et on affiche
# le top des fonctions consommatrices (en % du temps CPU du test).
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

# Benchmarks représentatifs avec args dimensionnés pour fournir
# suffisamment de samples (>~50 ms de CPU userspace par session).
# Format : "binaire args | nb_iterations_repetition"
BENCH_CMDS=(
    "./tests/21-create-many 5000|20"
    "./tests/22-create-many-recursive 2000|20"
    "./tests/23-create-many-once 5000|20"
    "./tests/31-switch-many 1000 1000|30"
    "./tests/32-switch-many-join 1000 1000|30"
    "./tests/33-switch-many-cascade 200 200|20"
    "./tests/51-fibonacci 20|5"
    "./tests/61-mutex 1000|20"
    "./tests/65-sem-prodcons|20"
)

PERF_DATA=$(mktemp /tmp/perf_XXXXXX.data)
RESULT_TMP=$(mktemp /tmp/perf_result_XXXXXX.txt)
trap "rm -f '$PERF_DATA' '$RESULT_TMP'" EXIT

# Pour collecter les blocs LaTeX et les imprimer à la fin
LATEX_BUFFER=$(mktemp /tmp/perf_latex_XXXXXX.tex)
trap "rm -f '$PERF_DATA' '$RESULT_TMP' '$LATEX_BUFFER'" EXIT

profile_one() {
    local entry="$1"
    local cmd="${entry%|*}"
    local reps="${entry##*|}"
    [ "$reps" = "$entry" ] && reps=1
    local bin
    bin=$(echo "$cmd" | awk '{print $1}')
    local label
    label=$(basename "$bin")

    if [ ! -x "$bin" ]; then
        echo "  [SKIP] $bin"
        return
    fi

    : > "$PERF_DATA"
    perf record --all-user -q -F 5000 -o "$PERF_DATA" \
        -- bash -c "export LD_LIBRARY_PATH='$ROOT_DIR'; cd '$ROOT_DIR'; for i in \$(seq 1 $reps); do $cmd >/dev/null 2>&1; done" \
        >/dev/null 2>&1 || {
        echo "  [ERREUR] perf record a echoue pour $cmd"
        return
    }

    # On ne garde que les samples du processus du test (filtrage par command name).
    # Le label est tronqué à 15 caractères par perf (limite kernel comm[16]).
    local short_label
    short_label="${label:0:15}"

    perf report -i "$PERF_DATA" --stdio --no-children -g none \
        --percent-limit 0.01 2>/dev/null | \
    awk -v TOP=12 -v THRESH=0.5 -v CMD="$short_label" '
    /^[[:space:]]+[0-9]+\.[0-9]+%/ {
        if ($2 != CMD) next
        # Reconstituer le symbole : tout ce qui suit "[.]" ou "[k]"
        sym = ""
        for (i = 1; i <= NF; i++) {
            if ($i == "[.]" || $i == "[k]") {
                for (j = i+1; j <= NF; j++) sym = (sym == "" ? $j : sym " " $j)
                break
            }
        }
        if (sym == "") next
        if (sym ~ /^0x[0-9a-f]+$/) next
        if (sym ~ /^\[/)           next
        if (sym == "__libc_start_main" || sym == "_start") next
        pct = $1; gsub(/%/, "", pct); pct += 0
        sums[sym] += pct
        objs[sym]  = $3
    }
    END {
        if (length(sums) == 0) { print "NO_DATA"; exit }
        n = 0
        for (f in sums) {
            if (sums[f] < THRESH) continue
            n++; fns[n] = f; pcts[n] = sums[f]; obs[n] = objs[f]
        }
        for (i=1; i<=n; i++)
            for (j=i+1; j<=n; j++)
                if (pcts[j] > pcts[i]) {
                    t=pcts[i]; pcts[i]=pcts[j]; pcts[j]=t
                    t=fns[i];  fns[i]=fns[j];   fns[j]=t
                    t=obs[i];  obs[i]=obs[j];   obs[j]=t
                }
        if (n > TOP) n = TOP
        for (i=1; i<=n; i++)
            printf "%s\t%s\t%.2f\n", fns[i], obs[i], pcts[i]
    }
    ' > "$RESULT_TMP"

    echo ""
    echo "================================================================="
    printf "   %s\n" "$cmd"
    echo "   TOP 12 FONCTIONS PAR TEMPS CPU  (seuil >= 0.5%)"
    echo "================================================================="
    echo ""

    if [ ! -s "$RESULT_TMP" ] || grep -q "^NO_DATA$" "$RESULT_TMP"; then
        echo "  Pas de donnees significatives pour ce test."
        return
    fi

    printf "%-38s  %-22s  %11s\n" "Fonction" "Module" "% temps CPU"
    printf "%-38s  %-22s  %11s\n" "--------------------------------------" "----------------------" "-----------"
    while IFS=$'\t' read -r fn obj pct; do
        printf "%-38s  %-22s  %10s%%\n" "$fn" "$obj" "$pct"
    done < "$RESULT_TMP"

    if [ "$LATEX" = "1" ]; then
        {
            echo ""
            echo '\begin{table}[H]'
            echo '\centering'
            echo '\renewcommand{\arraystretch}{1.3}'
            echo '\begin{tabular}{llr}'
            echo '\toprule'
            echo '\textbf{Fonction} & \textbf{Module} & \textbf{\% temps CPU} \\'
            echo '\midrule'
            while IFS=$'\t' read -r fn obj pct; do
                fn_tex=$(printf '%s' "$fn" | sed 's/_/\\_/g; s/\$/\\$/g')
                obj_tex=$(printf '%s' "$obj" | sed 's/_/\\_/g; s/\$/\\$/g')
                printf '\\texttt{%s} & \\texttt{%s} & %s\\,\\%% \\\\\n' "$fn_tex" "$obj_tex" "$pct"
            done < "$RESULT_TMP"
            echo '\bottomrule'
            echo '\end{tabular}'
            cmd_tex=$(printf '%s' "$cmd" | sed 's/_/\\_/g')
            printf '\\caption{Profil d'\''exécution — \\texttt{%s}}\n' "$cmd_tex"
            label=$(printf '%s' "$cmd" | tr -c '[:alnum:]' '-' | tr -s '-' | sed 's/^-//;s/-$//')
            printf '\\label{tab:profile-%s}\n' "$label"
            echo '\end{table}'
        } >> "$LATEX_BUFFER"
    fi
}

echo "[*] Profilage par test..."
for cmd in "${BENCH_CMDS[@]}"; do
    profile_one "$cmd"
done

if [ "$LATEX" = "1" ] && [ -s "$LATEX_BUFFER" ]; then
    echo ""
    echo "================================================================="
    echo "   TABLES LATEX  (copiez-collez dans votre rapport)"
    echo "================================================================="
    cat "$LATEX_BUFFER"
fi

echo ""
echo "[*] Termine."
