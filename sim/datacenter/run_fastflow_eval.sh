#!/bin/bash
# run_fastflow_eval.sh
# Run FASTFLOW evaluation for all modes across incast and permutation workloads.
# Produces .fct files in results/fastflow/<mode>/ parallel to results/swift/.
#
# Usage: ./run_fastflow_eval.sh [--modes MODE,...] [--jobs N]
# Run from sim/datacenter/

set -euo pipefail
cd "$(dirname "$0")"

BIN="./htsim_fastflow"
TOPO_1024="topologies/fat_tree_1024_800g_os8.topo"
JOBS=6

# Modes to run (subset for speed; edit as needed)
MODES=("fastflow" "fastflow+eqds" "fastflow+eqds+mcc")
RA_QA_MODES=("fastflow" "fastflow+eqds+mcc")  # run with ra_qa=on for ablation

mkdir -p results/fastflow

# ──────────────────────────────────────────────────────────────────────────────
# Incast benchmarks (8-deg, 32-deg, 100-deg; key sizes for paper figs)
# ──────────────────────────────────────────────────────────────────────────────
run_incast() {
    local mode="$1"
    local degree="$2"
    local size="$3"
    local ra_qa="${4:-off}"

    local sizekib=$((size / 1024))
    local cm="connection_matrices/incast_1024n_${degree}deg_${sizekib}KiB.cm"
    [[ -f "$cm" ]] || { echo "[skip] missing $cm"; return 0; }

    local tag
    if [[ "$ra_qa" == "on" ]]; then
        tag="${mode//+/_}+ra_qa"
    else
        tag="${mode//+/_}"
    fi

    local out="results/fastflow/${tag}"
    mkdir -p "$out"
    local label="incast_${degree}deg_${sizekib}KiB"
    local fct="$out/${label}.fct"
    [[ -f "$fct" ]] && { echo "[cached] $tag/$label"; return 0; }

    local endtime
    endtime=$(python3 -c "print(max(5000, min(2000000, int($degree * $size * 8 / 800000000 * 1e6 * 20 + 5000))))")

    "$BIN" \
        -topo "$TOPO_1024" \
        -tm "$cm" \
        -mode "$mode" \
        -ra_qa "$ra_qa" \
        -end "$endtime" \
        > >(grep "^FCT" > "$fct") \
        2>/dev/null

    local n
    n=$(wc -l < "$fct")
    echo "[done] $tag/$label: ${n}/${degree}"
}
export -f run_incast
export BIN TOPO_1024

# Key incast sizes for paper Fig 5 equivalent
INCAST_SIZES=(4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432 67108864)
INCAST_DEGREES=(8 32 100)

echo "=== FASTFLOW Incast Benchmarks ==="
CMDS=()
for mode in "${MODES[@]}"; do
    for deg in "${INCAST_DEGREES[@]}"; do
        for siz in "${INCAST_SIZES[@]}"; do
            CMDS+=("$mode $deg $siz off")
        done
    done
done
# RA-QA ablation on 32-deg and 100-deg (key comparison sizes)
for mode in "${RA_QA_MODES[@]}"; do
    for deg in 32 100; do
        for siz in 131072 262144 524288 1048576 2097152 4194304; do
            CMDS+=("$mode $deg $siz on")
        done
    done
done

printf '%s\n' "${CMDS[@]}" | xargs -P "$JOBS" -L 1 bash -c 'run_incast $@' _

# ──────────────────────────────────────────────────────────────────────────────
# Permutation benchmarks (2MiB — main comparison point)
# ──────────────────────────────────────────────────────────────────────────────
run_perm() {
    local mode="$1"
    local ra_qa="${2:-off}"

    local tag
    if [[ "$ra_qa" == "on" ]]; then
        tag="${mode//+/_}+ra_qa"
    else
        tag="${mode//+/_}"
    fi

    local out="results/fastflow/${tag}"
    mkdir -p "$out"

    # 2MiB permutation (1024 nodes, 8:1 OS)
    local cm="connection_matrices/perm_1024n_1024c_2MiB.cm"
    [[ -f "$cm" ]] || { echo "[skip] missing $cm"; return 0; }

    local fct="$out/perm_os8_2MiB.fct"
    if [[ ! -f "$fct" ]]; then
        "$BIN" \
            -topo "$TOPO_1024" \
            -tm "$cm" \
            -mode "$mode" \
            -ra_qa "$ra_qa" \
            -end 5000000 \
            > >(grep "^FCT" > "$fct") \
            2>/dev/null
        local n; n=$(wc -l < "$fct")
        echo "[done] $tag/perm_os8_2MiB: ${n}/1024"
    else
        echo "[cached] $tag/perm_os8_2MiB"
    fi
}
export -f run_perm
export BIN TOPO_1024

echo ""
echo "=== FASTFLOW Permutation Benchmarks ==="
PERM_CMDS=()
for mode in "${MODES[@]}"; do
    PERM_CMDS+=("$mode off")
done
for mode in "${RA_QA_MODES[@]}"; do
    PERM_CMDS+=("$mode on")
done

printf '%s\n' "${PERM_CMDS[@]}" | xargs -P "$JOBS" -L 1 bash -c 'run_perm $@' _

echo ""
echo "=== Summary ==="
echo "Incast completions:"
for tag_dir in results/fastflow/*/; do
    tag=$(basename "$tag_dir")
    total=$(ls "$tag_dir"incast_*.fct 2>/dev/null | wc -l)
    [[ "$total" -gt 0 ]] && echo "  $tag: $total scenarios"
done
echo "Permutation completions:"
for tag_dir in results/fastflow/*/; do
    tag=$(basename "$tag_dir")
    [[ -f "${tag_dir}perm_os8_2MiB.fct" ]] && {
        n=$(wc -l < "${tag_dir}perm_os8_2MiB.fct")
        echo "  $tag/perm_os8_2MiB: ${n}/1024"
    }
done
