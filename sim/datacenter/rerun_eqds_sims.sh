#!/bin/bash
# rerun_eqds_sims.sh
# Re-runs all FASTFLOW+EQDS* simulations after the initial-burst fix.
# Only protocols that use credits are affected: fastflow+eqds, fastflow+eqds+mcc, fastflow+eqds+mcc+coflow
# Swift, vanilla fastflow, and standalone eqds are NOT affected and keep their cached results.

set -uo pipefail
cd "$(dirname "$0")"

FF_BIN="./htsim_fastflow"
RESULTS="results/paper"

# Protocols to re-run (all use EQDS credits)
PROTOS=("fastflow+eqds" "fastflow+eqds+mcc+coflow")
PROTO_DIRS=("fastflow_eqds" "fastflow_eqds_mcc_coflow")

LINKSPEED_MBPS=800000
LINKSPEED_BPS=800000000000

run_ff() {
    local mode="$1" label="$2" topo="$3" cm="$4" endtime="$5" outdir="$6"
    local fct="$outdir/${label}.fct"
    mkdir -p "$outdir"

    if [ -s "$fct" ]; then
        echo "[cached] $mode/$label ($(wc -l < "$fct") flows)"
        return 0
    fi
    if [ ! -f "$cm" ]; then
        echo "[skip] missing $cm"
        return 0
    fi

    echo "[running] $mode / $label ..."
    "$FF_BIN" \
        -topo "$topo" \
        -tm "$cm" -mtu 4096 -q 500 \
        -mode "$mode" -plb on -trimming on \
        -end "$endtime" -o /dev/null \
        > >(grep "^FCT" > "$fct") 2>/dev/null

    local n
    n=$(wc -l < "$fct" 2>/dev/null || echo 0)
    echo "[done] $mode/$label: $n flows"
}

echo "=============================================="
echo "  Re-running EQDS credit simulations"
echo "  After initial-burst fix (BDP -> 2 MTU)"
echo "  Start: $(date)"
echo "=============================================="
echo ""

# ─── INCAST ──────────────────────────────────────────────────────────────────
echo "=== INCAST (14 sizes × 3 degrees × ${#PROTOS[@]} protocols) ==="
TOPO_NB="topologies/fat_tree_1024_800g_nb.topo"

for pi in "${!PROTOS[@]}"; do
    proto="${PROTOS[$pi]}"
    pdir="${PROTO_DIRS[$pi]}"
    outdir="$RESULTS/incast/$pdir"
    mkdir -p "$outdir"

    for deg in 8 32 100; do
        for siz_kib in 4 8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768; do
            siz=$((siz_kib * 1024))
            cm="connection_matrices/incast_1024n_${deg}deg_${siz_kib}KiB.cm"
            # endtime: scale with data volume, minimum 5000us
            end=$(python3 -c "print(max(5000, min(2000000, int($deg * $siz * 8 / $LINKSPEED_BPS * 1e6 * 20 + 5000))))")
            run_ff "$proto" "incast_${deg}deg_${siz_kib}KiB" "$TOPO_NB" "$cm" "$end" "$outdir"
        done
    done
done

echo ""
# ─── PERMUTATION ─────────────────────────────────────────────────────────────
echo "=== PERMUTATION ==="
TOPO_OS2="topologies/fat_tree_1024_800g_os2.topo"
TOPO_OS4="topologies/fat_tree_1024_800g_os4.topo"
TOPO_OS8="topologies/fat_tree_1024_800g_os8.topo"

for pi in "${!PROTOS[@]}"; do
    proto="${PROTOS[$pi]}"
    pdir="${PROTO_DIRS[$pi]}"
    outdir="$RESULTS/permutation/$pdir"
    mkdir -p "$outdir"

    # Fig 8 scenarios
    run_ff "$proto" "perm_os2_2MiB"   "$TOPO_OS2" "connection_matrices/perm_1024n_1024c_2MiB.cm"   2000000 "$outdir"
    run_ff "$proto" "perm_os2_32MiB"  "$TOPO_OS2" "connection_matrices/perm_1024n_1024c_32MiB_bisect.cm" 5000000 "$outdir"
    run_ff "$proto" "perm_os4_2MiB"   "$TOPO_OS4" "connection_matrices/perm_1024n_1024c_2MiB.cm"   2000000 "$outdir"
    run_ff "$proto" "perm_os4_32MiB"  "$TOPO_OS4" "connection_matrices/perm_1024n_1024c_32MiB_bisect.cm" 5000000 "$outdir"
    run_ff "$proto" "perm_os8_2MiB"   "$TOPO_OS8" "connection_matrices/perm_1024n_1024c_2MiB.cm"   2000000 "$outdir"
    run_ff "$proto" "perm_os8_32MiB"  "$TOPO_OS8" "connection_matrices/perm_1024n_1024c_32MiB_bisect.cm" 5000000 "$outdir"
    run_ff "$proto" "perm_os8_2MiB_bisect"   "$TOPO_OS8" "connection_matrices/perm_1024n_1024c_2MiB_bisect.cm"   2000000 "$outdir"
    run_ff "$proto" "perm_os8_2MiB_one4MiB"  "$TOPO_OS8" "connection_matrices/perm_1024n_1024c_2MiB_one4MiB_bisect.cm" 2000000 "$outdir"
    run_ff "$proto" "perm_os8_4x4MiB"        "$TOPO_OS8" "connection_matrices/perm_1024n_1024c_4x4MiB_bisect.cm"       3000000 "$outdir"
done

echo ""
# ─── ALL-TO-ALL ──────────────────────────────────────────────────────────────
echo "=== ALL-TO-ALL ==="
TOPO_128="topologies/fat_tree_128_800g_os8.topo"

for pi in "${!PROTOS[@]}"; do
    proto="${PROTOS[$pi]}"
    pdir="${PROTO_DIRS[$pi]}"
    outdir="$RESULTS/alltoall/$pdir"
    mkdir -p "$outdir"

    for k in 1 2 8 16; do
        cm="connection_matrices/a2a_128n_1MiB_k${k}.cm"
        run_ff "$proto" "a2a_128n_k${k}" "$TOPO_128" "$cm" 5000000 "$outdir"
    done
done

echo ""
echo "=============================================="
echo "  All simulations complete!"
echo "  End: $(date)"
echo "=============================================="
echo ""

# Regenerate plots
echo "Regenerating plots..."
python3 plot_paper_figures.py
echo ""
echo "=== Done! Check plots/paper/ for updated figures ==="
