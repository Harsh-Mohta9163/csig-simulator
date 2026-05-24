#!/bin/bash
# Re-run ALL 45 incast scenarios with corrected parameters:
#   -subflows 1   : reduces receiver-end congestion collapse
#   -cwnd 1       : 1-packet initial window prevents burst flooding
#   endtime: max(5000, min(2000000, degree*size*8/BW*1e6*20+5000))
#
# Run from sim/datacenter/

set -euo pipefail
cd "$(dirname "$0")"

BIN="./htsim_swift"
TOPO="topologies/fat_tree_1024_800g_nb.topo"
OUT="results/swift"
JOBS=8   # parallel jobs

mkdir -p "$OUT"

run_incast() {
    local degree="$1"
    local size="$2"
    local sizekib=$((size / 1024))
    local cm="connection_matrices/incast_1024n_${degree}deg_${sizekib}KiB.cm"
    local label="incast_${degree}deg_${sizekib}KiB"
    local fct="$OUT/${label}.fct"
    local log="$OUT/${label}.log"

    # Compute endtime: degree * size * 8/BW * 1e6 * 20 + 5000 us, capped at 2s
    local endtime
    endtime=$(python3 -c "print(max(5000, min(2000000, int($degree * $size * 8 / 800000000 * 1e6 * 20 + 5000))))")

    local tmpdat
    tmpdat=$(mktemp /tmp/htsim_XXXXXX.dat)

    "$BIN" -topo "$TOPO" \
        -tm "$cm" \
        -mtu 4096 -q 500 \
        -end "$endtime" \
        -plb on -subflows 1 -cwnd 1 \
        -o "$tmpdat" \
        > >(grep "^FCT" > "$fct") \
        2>"$log"
    rm -f "$tmpdat"

    local n
    n=$(wc -l < "$fct")
    echo "[done] $label: ${n}/${degree} (endtime=${endtime}us)"
}
export -f run_incast
export BIN TOPO OUT

SIZES=(4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432 67108864)

echo "=== Running all incast scenarios (Parallel: $JOBS) ==="
CMDS=()
for DEGREE in 8 32 100; do
    for SIZE in "${SIZES[@]}"; do
        CMDS+=("$DEGREE $SIZE")
    done
done

printf '%s\n' "${CMDS[@]}" | xargs -P "$JOBS" -L 1 bash -c 'run_incast $@' _

echo ""
echo "=== Summary ==="
wc -l "$OUT"/incast_*.fct | sort -k1 -n | grep -v "total"
