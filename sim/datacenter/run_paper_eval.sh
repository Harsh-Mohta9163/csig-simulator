#!/bin/bash
# run_paper_eval.sh
# Run paper scenarios from Bonato et al. 2024 (FASTFLOW paper).
# Protocols: swift, eqds, fastflow, fastflow+eqds
# All protocols: trimming ON, PLB/ECMP ON (equal footing, paper Sec 4).
# All 800Gbps link speed (-linkspeed 800000 passed to every binary).
#
# Topologies:
#   Incast      -> non-blocking 1024-node (paper: "Incasts run without oversubscription")
#   Permutation -> 2:1/4:1/8:1 oversubscribed 1024-node (Fig 8, Fig 10)
#
# Figures:
#   Fig 5   : incast relative FCT (8/32/100-deg x 14 sizes)
#   Fig 8   : permutation CDF (different OS ratios and scenarios)
#   Fig 10  : EQDS augmented permutation (2MiB and 32MiB)
#
# Usage: ./run_paper_eval.sh [--jobs N]
# Run from sim/datacenter/

set -uo pipefail
cd "$(dirname "$0")"

trap 'find . -name "logout.dat" -delete 2>/dev/null; true' EXIT

FF_BIN="./htsim_fastflow"
SW_BIN="./htsim_swift"
EQ_BIN="./htsim_eqds"
JOBS=4
RESULTS="results/paper"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "Unknown arg $1"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS"/{incast,permutation}/{swift,eqds,fastflow,fastflow_eqds}

TOPO_NB="topologies/fat_tree_1024_800g_nb.topo"
TOPO_OS2="topologies/fat_tree_1024_800g_os2.topo"
TOPO_OS4="topologies/fat_tree_1024_800g_os4.topo"
TOPO_OS8="topologies/fat_tree_1024_800g_os8.topo"

endtime_us() {
    local deg="$1" siz="$2"
    python3 -c "print(max(5000, min(2000000, int($deg * $siz * 8 / 800000000 * 1e6 * 20 + 5000))))"
}
export -f endtime_us

# ─────────────────────────────────────────────────────────────────────────────
# INCAST (Fig 5) — NON-BLOCKING 1024-node topology
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
            -mtu 4096 -q 500 -linkspeed 800000 \
            -plb on -subflows 1 -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    elif [[ "$proto" == "eqds" ]]; then
        "$EQ_BIN" \
            -topo "$TOPO_NB" \
            -tm "$cm" \
            -mtu 4096 -q 500 -linkspeed 800000 \
            -cwnd 1 \
            -strat ecmp_host -queue_type composite \
            -fct_log \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null || true
    else
        "$FF_BIN" \
            -topo "$TOPO_NB" \
            -tm "$cm" \
            -mtu 4096 -q 500 -linkspeed 800000 \
            -mode "$proto" \
            -plb on -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct")
    echo "[done] incast/$proto/$label: ${n}/${deg}"
}
export -f run_incast
export FF_BIN SW_BIN EQ_BIN TOPO_NB RESULTS

echo "=== Fig 5: Incast (non-blocking, 800Gbps) ==="
INCAST_CMDS=()
for proto in swift eqds fastflow "fastflow+eqds"; do
    for deg in 8 32 100; do
        for siz in 4 8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768; do
            INCAST_CMDS+=("$proto $deg $siz")
        done
    done
done
printf '%s\n' "${INCAST_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_incast $@' _ {}

# ─────────────────────────────────────────────────────────────────────────────
# PERMUTATION (Fig 8, 10) — oversubscribed 1024-node
# run_perm proto label cm topo_file endtime_us
# ─────────────────────────────────────────────────────────────────────────────
run_perm() {
    local proto="$1" label="$2" cm="$3" topo="$4" end="$5"
    [[ -f "$cm" ]] || { echo "[skip] missing $cm"; return 0; }

    local out="$RESULTS/permutation/${proto//+/_}"
    mkdir -p "$out"
    local fct="$out/${label}.fct"
    [[ -s "$fct" ]] && { echo "[cached] permutation/$proto/$label"; return 0; }

    if [[ "$proto" == "swift" ]]; then
        "$SW_BIN" \
            -topo "$topo" \
            -tm "$cm" \
            -mtu 4096 -q 500 -linkspeed 800000 \
            -plb on -subflows 1 -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    elif [[ "$proto" == "eqds" ]]; then
        "$EQ_BIN" \
            -topo "$topo" \
            -tm "$cm" \
            -mtu 4096 -q 500 -linkspeed 800000 \
            -cwnd 1 \
            -strat ecmp_host -queue_type composite \
            -fct_log \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null || true
    else
        "$FF_BIN" \
            -topo "$topo" \
            -tm "$cm" \
            -mode "$proto" \
            -mtu 4096 -q 500 -linkspeed 800000 \
            -plb on -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct")
    echo "[done] permutation/$proto/$label: ${n}"
}
export -f run_perm
export TOPO_OS2 TOPO_OS4 TOPO_OS8

CM_2M="connection_matrices/perm_1024n_1024c_2MiB.cm"
CM_32M="connection_matrices/perm_1024n_1024c_32MiB.cm"
CM_2M_BISECT="connection_matrices/perm_1024n_1024c_2MiB_bisect.cm"
CM_2M_ONE4M="connection_matrices/perm_1024n_1024c_2MiB_one4MiB.cm"
CM_4X4M="connection_matrices/perm_1024n_1024c_4x4MiB.cm"
export CM_2M CM_32M CM_2M_BISECT CM_2M_ONE4M CM_4X4M

echo ""
echo "=== Fig 8/10: Permutation (oversubscribed, 800Gbps) ==="
PERM_CMDS=()
for proto in swift eqds fastflow "fastflow+eqds"; do
    PERM_CMDS+=("$proto perm_os2_2MiB          $CM_2M        $TOPO_OS2 500000")
    PERM_CMDS+=("$proto perm_os2_32MiB         $CM_32M       $TOPO_OS2 3000000")
    PERM_CMDS+=("$proto perm_os4_2MiB          $CM_2M        $TOPO_OS4 500000")
    PERM_CMDS+=("$proto perm_os4_32MiB         $CM_32M       $TOPO_OS4 3000000")
    PERM_CMDS+=("$proto perm_os8_2MiB          $CM_2M        $TOPO_OS8 500000")
    PERM_CMDS+=("$proto perm_os8_32MiB         $CM_32M       $TOPO_OS8 3000000")
    PERM_CMDS+=("$proto perm_os8_4x4MiB        $CM_4X4M      $TOPO_OS8 1000000")
    PERM_CMDS+=("$proto perm_os8_2MiB_one4MiB  $CM_2M_ONE4M  $TOPO_OS8 1000000")
    PERM_CMDS+=("$proto perm_os8_2MiB_bisect   $CM_2M_BISECT $TOPO_OS8 500000")
done
printf '%s\n' "${PERM_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_perm $@' _ {}

# Alltoall evaluation is paused — not included in current run.

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Summary ==="
for wkld in incast permutation; do
    echo "  $wkld:"
    for d in "$RESULTS/$wkld"/*/; do
        proto=$(basename "$d")
        n=$(ls "$d"*.fct 2>/dev/null | wc -l)
        [[ "$n" -gt 0 ]] && echo "    $proto: $n scenarios"
    done
done
