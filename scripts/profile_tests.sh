#!/bin/bash
# profile_tests.sh — Profil CPU par test, pour identifier les bottlenecks.
#
# Usage : ./scripts/profile_tests.sh [--latex] [--show-dl]
#   --latex     Génère aussi une table LaTeX par test.
#   --show-dl   N'agrège pas les fonctions du linker dynamique (ld-linux).
#               Par défaut, elles sont regroupées sous "[dynamic linker]"
#               car elles polluent les benchmarks courts.
#
# Pour chaque benchmark on lance une session perf isolée et on affiche
# le top des fonctions consommatrices (en % du temps CPU userspace du test).
# Les fonctions marquées d'un * appartiennent au binaire de test ou à
# libthread.so : c'est *notre* code, là où l'optimisation doit porter.
#
# Nécessite : perf (linux-tools-common / linux-tools-$(uname -r))

LATEX=0
SHOW_DL=0
for arg in "$@"; do
    case "$arg" in
        --latex)   LATEX=1 ;;
        --show-dl) SHOW_DL=1 ;;
    esac
done

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

# Benchmarks représentatifs.
# Format : "binaire args|nb_repetitions"
# On dimensionne les arguments ET les répétitions pour que chaque session
# accumule >= ~200 ms de CPU userspace utile (signal stable).
BENCH_CMDS=(
    "./tests/21-create-many 5000|50"
    "./tests/22-create-many-recursive 2000|50"
    "./tests/23-create-many-once 5000|50"
    "./tests/31-switch-many 1000 1000|50"
    "./tests/32-switch-many-join 1000 1000|50"
    "./tests/33-switch-many-cascade 200 200|30"
    "./tests/51-fibonacci 20|10"
    "./tests/61-mutex 1000|50"
    "./tests/65-sem-prodcons|50"
)

PERF_DATA=$(mktemp /tmp/perf_XXXXXX.data)
RESULT_TMP=$(mktemp /tmp/perf_result_XXXXXX.txt)
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
    # LD_BIND_NOW=1 pré-résout tous les symboles au démarrage : on retire
    # la résolution paresseuse du chemin chaud et on amortit le coût du
    # linker sur la première itération seulement.
    perf record --all-user -q -F 5000 -o "$PERF_DATA" \
        -- bash -c "
            export LD_LIBRARY_PATH='$ROOT_DIR'
            export LD_BIND_NOW=1
            cd '$ROOT_DIR'
            for i in \$(seq 1 $reps); do $cmd >/dev/null 2>&1; done
        " >/dev/null 2>&1 || {
        echo "  [ERREUR] perf record a echoue pour $cmd"
        return
    }

    # perf tronque le command name à 15 caractères (kernel comm[16]).
    local short_label="${label:0:15}"

    perf report -i "$PERF_DATA" --stdio --no-children -g none \
        --percent-limit 0.01 2>/dev/null | \
    awk -v TOP=12 -v THRESH=0.3 -v CMD="$short_label" \
        -v BIN="$label" -v SHOW_DL="$SHOW_DL" '
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
        if (sym == "")             next
        if (sym ~ /^0x[0-9a-f]+$/) next
        if (sym ~ /^\[/)           next
        if (sym == "__libc_start_main" || sym == "_start") next

        pct = $1; gsub(/%/, "", pct); pct += 0
        obj = $3

        # Agrégation du linker dynamique (ld-linux*) sauf si --show-dl
        if (SHOW_DL == 0 && obj ~ /^ld-linux/) {
            sums["[dynamic linker]"] += pct
            objs["[dynamic linker]"]  = "ld-linux-x86-64.so.2"
            cats["[dynamic linker]"]  = "LD"
            next
        }

        # Catégorisation : USER = notre code (binaire test ou libthread.so)
        cat = "EXT"
        if (obj == BIN || obj == "libthread.so") cat = "USER"
        else if (obj ~ /^libc/)                   cat = "LIBC"
        else if (obj ~ /^ld-linux/)               cat = "LD"

        sums[sym] += pct
        objs[sym]  = obj
        cats[sym]  = cat
    }
    END {
        if (length(sums) == 0) { print "NO_DATA"; exit }
        n = 0; total = 0
        for (f in sums) {
            total += sums[f]
            if (sums[f] < THRESH) continue
            n++; fns[n] = f; pcts[n] = sums[f]
            obs[n] = objs[f]; cs[n]  = cats[f]
        }
        # Tri décroissant par %
        for (i=1; i<=n; i++)
            for (j=i+1; j<=n; j++)
                if (pcts[j] > pcts[i]) {
                    t=pcts[i]; pcts[i]=pcts[j]; pcts[j]=t
                    t=fns[i];  fns[i]=fns[j];   fns[j]=t
                    t=obs[i];  obs[i]=obs[j];   obs[j]=t
                    t=cs[i];   cs[i]=cs[j];     cs[j]=t
                }
        if (n > TOP) n = TOP
        printf "TOTAL\t%.2f\n", total
        for (i=1; i<=n; i++)
            printf "%s\t%s\t%s\t%.2f\n", cs[i], fns[i], obs[i], pcts[i]
    }
    ' > "$RESULT_TMP"

    echo ""
    echo "================================================================="
    printf "   %s  (x%d)\n" "$cmd" "$reps"
    echo "   TOP 12 FONCTIONS PAR TEMPS CPU USERSPACE  (seuil >= 0.3%)"
    echo "   * = notre code (binaire de test ou libthread.so)"
    echo "================================================================="
    echo ""

    if [ ! -s "$RESULT_TMP" ] || grep -q "^NO_DATA$" "$RESULT_TMP"; then
        echo "  Pas de donnees significatives pour ce test."
        return
    fi

    # Lire le total et somme USER
    local total user_sum
    total=$(awk -F'\t' '$1=="TOTAL"{print $2}' "$RESULT_TMP")
    user_sum=$(awk -F'\t' '$1=="USER"{s+=$4} END{printf "%.2f", s+0}' "$RESULT_TMP")
    printf "   Couverture userspace : %s%%   |   Notre code (* USER) : %s%%\n" \
        "$total" "$user_sum"
    echo ""

    printf "    %-36s  %-22s  %11s\n" "Fonction" "Module" "% temps CPU"
    printf "    %-36s  %-22s  %11s\n" \
        "------------------------------------" \
        "----------------------" "-----------"
    awk -F'\t' '$1!="TOTAL"' "$RESULT_TMP" | \
    while IFS=$'\t' read -r cat fn obj pct; do
        local marker=" "
        [ "$cat" = "USER" ] && marker="*"
        printf "  %s %-36s  %-22s  %10s%%\n" "$marker" "$fn" "$obj" "$pct"
    done

    if [ "$LATEX" = "1" ]; then
        {
            echo ""
            echo '\begin{table}[H]'
            echo '\centering'
            echo '\renewcommand{\arraystretch}{1.3}'
            echo '\begin{tabular}{cllr}'
            echo '\toprule'
            echo ' & \textbf{Fonction} & \textbf{Module} & \textbf{\% CPU} \\'
            echo '\midrule'
            awk -F'\t' '$1!="TOTAL"' "$RESULT_TMP" | \
            while IFS=$'\t' read -r cat fn obj pct; do
                local mark=""
                [ "$cat" = "USER" ] && mark='$\star$'
                fn_tex=$(printf '%s' "$fn" | sed 's/_/\\_/g; s/\$/\\$/g')
                obj_tex=$(printf '%s' "$obj" | sed 's/_/\\_/g; s/\$/\\$/g')
                printf '%s & \\texttt{%s} & \\texttt{%s} & %s\\,\\%% \\\\\n' \
                    "$mark" "$fn_tex" "$obj_tex" "$pct"
            done
            echo '\bottomrule'
            echo '\end{tabular}'
            cmd_tex=$(printf '%s' "$cmd" | sed 's/_/\\_/g')
            printf '\\caption{Profil d'\''exécution — \\texttt{%s} (x%d). $\\star$ = code projet.}\n' \
                "$cmd_tex" "$reps"
            label=$(printf '%s' "$cmd" | tr -c '[:alnum:]' '-' | tr -s '-' | sed 's/^-//;s/-$//')
            printf '\\label{tab:profile-%s}\n' "$label"
            echo '\end{table}'
        } >> "$LATEX_BUFFER"
    fi
}

echo "[*] Profilage par test (LD_BIND_NOW=1, --all-user)..."
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
