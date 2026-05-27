#!/bin/bash
# run_extended_eval.sh
# Run missing scenarios for paper-style plots:
#   1. Full incast size sweep (4 KiB to 32768 KiB, all degrees)
#   2. Permutation bisect scenarios (Fig 8 and Fig 10)
#
# Protocols: swift, fastflow, fastflow+eqds, fastflow+eqds+mcc+coflow
# Usage: ./run_extended_eval.sh [--jobs N] [--incast-only] [--perm-only]

set -uo pipefail
cd "$(dirname "$0")"

trap 'find . -name "logout.dat" -delete 2>/dev/null; true' EXIT

FF_BIN="./htsim_fastflow"
SW_BIN="./htsim_swift"
JOBS=4
RESULTS="results/paper"
RUN_INCAST=1
RUN_PERM=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --incast-only) RUN_PERM=0; shift ;;
        --perm-only) RUN_INCAST=0; shift ;;
        *) echo "Unknown arg $1"; exit 1 ;;
    esac
done

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
# INCAST full sweep: all sizes 4 KiB to 32768 KiB (2^2 to 2^15)
# ─────────────────────────────────────────────────────────────────────────────
run_incast_size() {
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
            -topo "$TOPO_NB" -tm "$cm" \
            -mtu 4096 -q 500 \
            -plb on -subflows 1 -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    else
        "$FF_BIN" \
            -topo "$TOPO_NB" -tm "$cm" \
            -mtu 4096 -q 500 \
            -mode "$proto" -plb on -trimming on \
            -end "$end" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct" 2>/dev/null || echo 0)
    echo "[done] incast/$proto/${deg}deg/${siz_kib}KiB: ${n}/${deg}"
}
export -f run_incast_size
export FF_BIN SW_BIN TOPO_NB RESULTS

# ─────────────────────────────────────────────────────────────────────────────
# PERMUTATION bisect scenarios (Fig 8 + Fig 10)
# ─────────────────────────────────────────────────────────────────────────────
run_perm_bisect() {
    local proto="$1" scenario="$2" topo="$3" cm_file="$4" endtime="$5"
    [[ -f "$cm_file" ]] || { echo "[skip] missing $cm_file"; return 0; }

    local out="$RESULTS/permutation/${proto//+/_}"
    mkdir -p "$out"
    local fct="$out/${scenario}.fct"
    [[ -s "$fct" ]] && { echo "[cached] permutation/$proto/$scenario"; return 0; }

    if [[ "$proto" == "swift" ]]; then
        "$SW_BIN" \
            -topo "$topo" -tm "$cm_file" \
            -mtu 4096 -q 500 \
            -plb on -subflows 1 -trimming on \
            -end "$endtime" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    else
        "$FF_BIN" \
            -topo "$topo" -tm "$cm_file" \
            -mtu 4096 -q 500 \
            -mode "$proto" -plb on -trimming on \
            -end "$endtime" -o /dev/null \
            > >(grep "^FCT" > "$fct") 2>/dev/null
    fi
    local n; n=$(wc -l < "$fct" 2>/dev/null || echo 0)
    echo "[done] permutation/$proto/$scenario: ${n}"
}
export -f run_perm_bisect
export FF_BIN SW_BIN TOPO_OS2 TOPO_OS4 TOPO_OS8 RESULTS

# ─────────────────────────────────────────────────────────────────────────────
# Run incast sweep
# ─────────────────────────────────────────────────────────────────────────────
if [[ "$RUN_INCAST" == "1" ]]; then
    echo "=== Incast full size sweep (Fig 5) ==="
    INCAST_CMDS=()
    for proto in swift fastflow "fastflow+eqds" "fastflow+eqds+mcc+coflow"; do
        for deg in 8 32 100; do
            for siz in 4 8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768; do
                INCAST_CMDS+=("$proto $deg $siz")
            done
        done
    done
    printf '%s\n' "${INCAST_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_incast_size $@' _ {}
fi

# ─────────────────────────────────────────────────────────────────────────────
# Run permutation bisect scenarios
# ─────────────────────────────────────────────────────────────────────────────
if [[ "$RUN_PERM" == "1" ]]; then
    echo ""
    echo "=== Permutation bisect scenarios (Fig 8 + Fig 10) ==="

    # endtime: 32MiB flows on OS2/OS4/OS8 — allow up to 5s for all to complete
    # 4x4MiB and 2MiB+one4MiB — shorter flows, 2s sufficient
    PERM_CMDS=()
    for proto in swift fastflow "fastflow+eqds" "fastflow+eqds+mcc+coflow"; do
        # Fig 8(a): 2:1 OS, 32MiB bisect
        PERM_CMDS+=("$proto perm_os2_32MiB $TOPO_OS2 connection_matrices/perm_1024n_1024c_32MiB_bisect.cm 5000000")
        # Fig 8(b): 4:1 OS, 32MiB bisect
        PERM_CMDS+=("$proto perm_os4_32MiB $TOPO_OS4 connection_matrices/perm_1024n_1024c_32MiB_bisect.cm 5000000")
        # Fig 8(c): 8:1 OS, 4x4MiB bisect (windowed — 4 concurrent flows per node)
        PERM_CMDS+=("$proto perm_os8_4x4MiB $TOPO_OS8 connection_matrices/perm_1024n_1024c_4x4MiB_bisect.cm 3000000")
        # Fig 8(d): 8:1 OS, 2MiB+one 4MiB bisect
        PERM_CMDS+=("$proto perm_os8_2MiB_one4MiB $TOPO_OS8 connection_matrices/perm_1024n_1024c_2MiB_one4MiB_bisect.cm 2000000")
        # Fig 10(a): 8:1 OS, 2MiB bisect (homogeneous)
        PERM_CMDS+=("$proto perm_os8_2MiB_bisect $TOPO_OS8 connection_matrices/perm_1024n_1024c_2MiB_bisect.cm 2000000")
    done
    printf '%s\n' "${PERM_CMDS[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_perm_bisect $@' _ {}
fi

echo ""
echo "=== Extended eval complete ==="
echo "Re-run: python3 plot_paper_figures.py"
