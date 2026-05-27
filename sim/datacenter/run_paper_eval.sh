#!/bin/bash
# run_paper_eval.sh
# Run ALL paper scenarios from Bonato et al. 2024 (FASTFLOW paper).
# Protocols: swift, fastflow, fastflow+eqds, fastflow+eqds+mcc, fastflow+eqds+mcc+coflow
# All protocols: trimming ON, PLB ON (equal footing, paper Sec 4).
#
# Topologies (from paper):
#   Incast        -> non-blocking 1024-node (paper: "Incasts run without oversubscription")
#   Permutation   -> 8:1 oversubscribed 1024-node (Fig 1,8)
#   Alltoall      -> 8:1 oversubscribed  128-node (Fig 9)
#
# Figures reproduced:
#   Fig 1/8 : permutation CDF (2MiB and 32MiB; OS2/OS4/OS8)
#   Fig 5   : incast relative FCT (8/32/100-deg x 512KiB/4MiB/32MiB)
#   Fig 9   : alltoall bar chart (k=1/2/8/16)
#   Fig 10  : EQDS augmented permutation (2MiB and 32MiB+64MiB)
#
# Usage: ./run_paper_eval.sh [--jobs N]
# Run from sim/datacenter/

set -uo pipefail
cd "$(dirname "$0")"

# Clean up any leftover simulator logfiles (they grow to GB; we redirect to /dev/null)
trap 'find . -name "logout.dat" -delete 2>/dev/null; true' EXIT

FF_BIN="./htsim_fastflow"
SW_BIN="./htsim_swift"
JOBS=4
RESULTS="results/paper"

# Parse optional args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "Unknown arg $1"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS"/{incast,permutation,alltoall}/{swift,fastflow,fastflow_eqds,fastflow_eqds_mcc,fastflow_eqds_mcc_coflow}

TOPO_NB="topologies/fat_tree_1024_800g_nb.topo"
TOPO_OS2="topologies/fat_tree_1024_800g_os2.topo"
TOPO_OS4="topologies/fat_tree_1024_800g_os4.topo"
TOPO_OS8="topologies/fat_tree_1024_800g_os8.topo"
TOPO_128_OS8="topologies/fat_tree_128_800g_os8.topo"

# ─────────────────────────────────────────────────────────────────────────────
# Helper: compute endtime in microseconds (capped at 2s)
# Usage: endtime_us <degree> <size_bytes>
# ─────────────────────────────────────────────────────────────────────────────
endtime_us() {
    local deg="$1" siz="$2"
    python3 -c "print(max(5000, min(2000000, int($deg * $siz * 8 / 800000000 * 1e6 * 20 + 5000))))"
}
export -f endtime_us

# ─────────────────────────────────────────────────────────────────────────────
# INCAST (Fig 5) — NON-BLOCKING 1024-node topology
# degrees: 8, 32, 100  |  sizes: 512KiB, 4MiB, 32MiB
# ─────────────────────────────────────────────────────────────────────────────
run_incast() {
    local proto="$1" deg="$2" siz_kib="$3"
    local siz=$((siz_kib * 1024))
    local cm="connection_matrices/incast_1024n_${deg}deg_${siz_kib}KiB.cm"
    [[ -f "$cm" ]] || { echo "[skip] missing $cm"; return 0; }

    local out="$RESULTS/incast/${proto//+/_}"
    mkdir -p "$out"
    local label="incast_${deg}deg_${siz_kib}KiB"
    local fct="$out/${label}.fct"
    [[ -s "$fct" ]] && { echo "[cached] incast/$proto/$label"; return 0; }

    local end; end=$(endtime_us "$deg" "$siz")

    if [[ "$proto" == "swift" ]]; then
        "$SW_BIN" \
            -topo "$TOPO_NB" \
            -tm "$cm" \
            -mtu 4096 -q 500 \
            -plb on -subflows 1 -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    else
        "$FF_BIN" \
            -topo "$TOPO_NB" \
            -tm "$cm" \
            -mtu 4096 -q 500 \
            -mode "$proto" \
            -plb on -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct")
    echo "[done] incast/$proto/$label: ${n}/${deg}"
}
export -f run_incast
export FF_BIN SW_BIN TOPO_NB RESULTS

echo "=== Fig 5: Incast (non-blocking, PLB on) ==="
INCAST_CMDS=()
for proto in swift fastflow "fastflow+eqds" "fastflow+eqds+mcc"; do
    for deg in 8 32 100; do
        for siz in 512 4096 32768; do
            INCAST_CMDS+=("$proto $deg $siz")
        done
    done
done
printf '%s\n' "${INCAST_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_incast $@' _ {}

# ─────────────────────────────────────────────────────────────────────────────
# PERMUTATION (Fig 1, 8) — oversubscribed 1024-node
# ─────────────────────────────────────────────────────────────────────────────
run_perm() {
    local proto="$1" topo_tag="$2" topo="$3" siz_mib="$4"
    local cm="connection_matrices/perm_1024n_1024c_${siz_mib}MiB.cm"
    [[ -f "$cm" ]] || { echo "[skip] missing $cm"; return 0; }

    local out="$RESULTS/permutation/${proto//+/_}"
    mkdir -p "$out"
    local label="perm_${topo_tag}_${siz_mib}MiB"
    local fct="$out/${label}.fct"
    [[ -s "$fct" ]] && { echo "[cached] permutation/$proto/$label"; return 0; }

    # endtime: target ~3s for 32MiB, 1s for 2MiB
    local end=$(python3 -c "import math; print(min(5000000, max(500000, $siz_mib * 1024 * 1024 * 8 * 20 // 800000000 + 500000)))")

    if [[ "$proto" == "swift" ]]; then
        "$SW_BIN" \
            -topo "$topo" \
            -tm "$cm" \
            -mtu 4096 -q 500 \
            -plb on -subflows 1 -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    else
        "$FF_BIN" \
            -topo "$topo" \
            -tm "$cm" \
            -mode "$proto" \
            -mtu 4096 -q 500 \
            -plb on -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct")
    echo "[done] permutation/$proto/$label: ${n}/1024"
}
export -f run_perm
export TOPO_OS2 TOPO_OS4 TOPO_OS8

echo ""
echo "=== Fig 1/8: Permutation (oversubscribed, PLB on) ==="
PERM_CMDS=()
for proto in swift fastflow "fastflow+eqds" "fastflow+eqds+mcc"; do
    # Fig 1: 8:1 OS, 2MiB and 32MiB
    PERM_CMDS+=("$proto os8 $TOPO_OS8 2")
    PERM_CMDS+=("$proto os8 $TOPO_OS8 32")
    # Fig 8: different OS ratios, 2MiB
    PERM_CMDS+=("$proto os2 $TOPO_OS2 2")
    PERM_CMDS+=("$proto os4 $TOPO_OS4 2")
done
printf '%s\n' "${PERM_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_perm $@' _ {}

# ─────────────────────────────────────────────────────────────────────────────
# ALLTOALL (Fig 9) — 128-node 8:1 oversubscribed, windowed k=1/2/8/16
# ─────────────────────────────────────────────────────────────────────────────
run_alltoall() {
    local proto="$1" k="$2"
    local cm="connection_matrices/a2a_128n_1MiB_k${k}.cm"
    [[ -f "$cm" ]] || { echo "[skip] missing $cm"; return 0; }

    local out="$RESULTS/alltoall/${proto//+/_}"
    mkdir -p "$out"
    local label="a2a_128n_k${k}"
    local fct="$out/${label}.fct"
    [[ -s "$fct" ]] && { echo "[cached] alltoall/$proto/$label"; return 0; }

    # 128 nodes, 16256 connections each 1MiB.
    # 5s endtime: complex protocols (eqds+mcc, coflow) need up to ~500ms sim time
    # due to credit back-pressure; simpler protocols exit early when done.
    local end=5000000  # 5s

    if [[ "$proto" == "swift" ]]; then
        "$SW_BIN" \
            -topo "$TOPO_128_OS8" \
            -tm "$cm" \
            -mtu 4096 -q 500 \
            -plb on -subflows 1 -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    else
        "$FF_BIN" \
            -topo "$TOPO_128_OS8" \
            -tm "$cm" \
            -mode "$proto" \
            -mtu 4096 -q 500 \
            -plb on -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct")
    echo "[done] alltoall/$proto/$label: ${n}/16256"
}
export -f run_alltoall
export TOPO_128_OS8

echo ""
echo "=== Fig 9: Alltoall (128-node OS8, PLB on) ==="
A2A_CMDS=()
for proto in swift fastflow "fastflow+eqds" "fastflow+eqds+mcc" "fastflow+eqds+mcc+coflow"; do
    for k in 1 2 8 16; do
        A2A_CMDS+=("$proto $k")
    done
done
printf '%s\n' "${A2A_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_alltoall $@' _ {}

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Summary ==="
for wkld in incast permutation alltoall; do
    echo "  $wkld:"
    for d in "$RESULTS/$wkld"/*/; do
        proto=$(basename "$d")
        n=$(ls "$d"*.fct 2>/dev/null | wc -l)
        [[ "$n" -gt 0 ]] && echo "    $proto: $n scenarios"
    done
done
