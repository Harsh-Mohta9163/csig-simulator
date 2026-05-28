#!/bin/bash
# run_missing_for_plots.sh
# Runs ONLY the missing simulation data required to complete all 4 paper figures.
#
# Missing data:
#   Fig 8 (Perm CDF):  4 EQDS permutation scenarios
#   Fig 5, 9, 10:      Already complete!
#
# The EQDS binary uses different CLI flags than FASTFLOW.

set -uo pipefail
cd "$(dirname "$0")"

trap 'find . -name "logout.dat" -delete 2>/dev/null; true' EXIT

EQDS_BIN="./htsim_eqds"
RESULTS="results/paper"
TOPO_OS2="topologies/fat_tree_1024_800g_os2.topo"
TOPO_OS4="topologies/fat_tree_1024_800g_os4.topo"
TOPO_OS8="topologies/fat_tree_1024_800g_os8.topo"

run_eqds_perm() {
    local label="$1" topo="$2" cm="$3" endtime="$4"
    local out="$RESULTS/permutation/eqds"
    mkdir -p "$out"
    local fct="$out/${label}.fct"

    if [ -s "$fct" ]; then
        echo "[cached] eqds/$label ($(wc -l < "$fct") flows)"
        return 0
    fi
    if [ ! -f "$cm" ]; then
        echo "[skip] missing $cm"
        return 0
    fi

    echo "[running] eqds/$label ..."
    "$EQDS_BIN" \
        -topo "$topo" \
        -tm "$cm" \
        -mtu 4096 -q 500 \
        -strat ecmp_host -queue_type composite \
        -fct_log \
        -end "$endtime" \
        -o /dev/null \
        > >(grep "^FCT" > "$fct") 2>/dev/null

    local n
    n=$(wc -l < "$fct" 2>/dev/null || echo 0)
    echo "[done] eqds/$label: $n flows"
}

echo "=== Running missing EQDS permutation scenarios for Fig 8 ==="
echo "Start time: $(date)"
echo ""

# Fig 8(a): 2:1 OS, 32MiB
run_eqds_perm "perm_os2_32MiB" "$TOPO_OS2" \
    "connection_matrices/perm_1024n_1024c_32MiB_bisect.cm" 5000000

# Fig 8(b): 4:1 OS, 32MiB
run_eqds_perm "perm_os4_32MiB" "$TOPO_OS4" \
    "connection_matrices/perm_1024n_1024c_32MiB_bisect.cm" 5000000

# Fig 8(c): 8:1 OS, 4x4MiB
run_eqds_perm "perm_os8_4x4MiB" "$TOPO_OS8" \
    "connection_matrices/perm_1024n_1024c_4x4MiB_bisect.cm" 3000000

# Fig 8(d): 8:1 OS, 2MiB + one 4MiB
run_eqds_perm "perm_os8_2MiB_one4MiB" "$TOPO_OS8" \
    "connection_matrices/perm_1024n_1024c_2MiB_one4MiB_bisect.cm" 2000000

echo ""
echo "=== All missing simulations complete ==="
echo "End time: $(date)"
echo ""
echo "Now regenerating plots..."
python3 plot_paper_figures.py
echo ""
echo "=== Done! Check plots/paper/ for updated figures ==="
