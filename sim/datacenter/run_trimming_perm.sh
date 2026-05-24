#!/bin/bash
# Run permutation scenarios with packet trimming enabled
# Run from sim/datacenter/

cd "$(dirname "$0")"

SIMDIR="."
BIN="$SIMDIR/htsim_swift"
TOPO_DIR="topologies"
CM_DIR="connection_matrices"
OUT_DIR="results/swift"
MTU=4096
QUEUE_PKTS=500

mkdir -p "$OUT_DIR"

run_sim() {
    local label="$1"
    local topo="$2"
    local cm="$3"
    local extra_args="${4:-}"
    local outfile="$OUT_DIR/${label}.dat"
    local endtime_us="${5:-5000}"
    local fct_file="$OUT_DIR/${label}.fct"

    echo "  Running: $label"
    local tmpdat
    tmpdat=$(mktemp /tmp/htsim_XXXXXX.dat)
    $BIN \
        -topo "$topo" \
        -tm "$cm" \
        -mtu $MTU \
        -q $QUEUE_PKTS \
        -end $endtime_us \
        -o "$tmpdat" \
        $extra_args \
        > >(tee "${outfile%.dat}.log" | grep "^FCT" > "$fct_file") \
        2>>"${outfile%.dat}.log"
    rm -f "$tmpdat"
    echo "  -> $fct_file ($(wc -l < "$fct_file") FCTs)"
}

TOPO_OS8="$TOPO_DIR/fat_tree_1024_800g_os8.topo"
TOPO_OS4="$TOPO_DIR/fat_tree_1024_800g_os4.topo"
TOPO_OS2="$TOPO_DIR/fat_tree_1024_800g_os2.topo"

echo "=== Permutation with Packet Trimming ==="

run_sim "perm_os8_2MiB_trim"         "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_2MiB_bisect.cm"          "-plb on -subflows 2 -trimming on" 3000
run_sim "perm_os8_32MiB_trim"        "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm"         "-plb off -subflows 8 -trimming on" 8000

run_sim "perm_os2_32MiB_trim"        "$TOPO_OS2" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm"         "-plb off -subflows 8 -trimming on" 5000
run_sim "perm_os4_32MiB_trim"        "$TOPO_OS4" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm"         "-plb off -subflows 8 -trimming on" 5000
run_sim "perm_os8_4x4MiB_trim"       "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_4x4MiB_bisect.cm"        "-plb on -subflows 2 -trimming on" 5000
run_sim "perm_os8_2MiB_one4MiB_trim" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_2MiB_one4MiB_bisect.cm" "-plb on -subflows 2 -trimming on" 5000

echo ""
echo "=== Done. Trimming FCT files in $OUT_DIR ==="
