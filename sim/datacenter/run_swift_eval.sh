#!/bin/bash
# Run all Swift evaluation scenarios from the paper
# Builds the simulator and runs each experiment, writing output to results/
# Run from sim/datacenter/

# Do not use set -e: a single sim crash should not abort the whole eval
cd "$(dirname "$0")"

SIMDIR="."
BIN="$SIMDIR/htsim_swift"
TOPO_DIR="topologies"
CM_DIR="connection_matrices"
OUT_DIR="results/swift"
MTU=4096
QUEUE_PKTS=500           # Swift needs large queues at 800Gbps to avoid drops (8/100 too small)
                         # 500 packets ≈ 2MB buffer per link, enough for Swift's ~20us target delay

mkdir -p "$OUT_DIR"

# make sure we're built
echo "=== Building ==="
(cd .. && make libhtsim.a -j$(nproc))
make htsim_swift -j$(nproc)

echo "=== Generating matrices (if not present) ==="
(cd "$CM_DIR" && bash generate_eval_matrices.sh)

run_sim() {
    local label="$1"
    local topo="$2"
    local cm="$3"
    local extra_args="${4:-}"
    local outfile="$OUT_DIR/${label}.dat"
    local endtime_us="${5:-5000}"   # default 5ms sim time
    local fct_file="$OUT_DIR/${label}.fct"

    echo "  Running: $label"
    # Use a unique temp file per run (logfile.cpp reads back the file to transpose
    # it, so /dev/null doesn't work).  Delete immediately after to save disk.
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
    echo "  -> $fct_file"
}

# ============================================================
echo ""
echo "=== Section 4.3: Permutation (Fig 1 & Fig 8) ==="
# ============================================================

TOPO_OS8="$TOPO_DIR/fat_tree_1024_800g_os8.topo"
TOPO_OS4="$TOPO_DIR/fat_tree_1024_800g_os4.topo"
TOPO_OS2="$TOPO_DIR/fat_tree_1024_800g_os2.topo"

# Fig 1(a): 8:1 OS, 2MiB - bisect permutation (no intra-pod flows, matching paper definition)
run_sim "perm_os8_2MiB"  "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_2MiB_bisect.cm"  "-plb on -subflows 2" 3000

# Fig 1(b): 8:1 OS, 32MiB - bisect permutation
run_sim "perm_os8_32MiB" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm" "-plb on -subflows 2" 8000

# Fig 8a: 2:1 OS, 32MiB - bisect permutation
run_sim "perm_os2_32MiB" "$TOPO_OS2" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm" "-plb on -subflows 2" 5000

# Fig 8b: 4:1 OS, 32MiB - bisect permutation
run_sim "perm_os4_32MiB" "$TOPO_OS4" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm" "-plb on -subflows 2" 5000

# Fig 8c: 8:1 OS, 4x4MiB parallel permutations (4 concurrent bisect permutations)
run_sim "perm_os8_4x4MiB" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_4x4MiB_bisect.cm" "-plb on -subflows 2" 5000

# Fig 8d: 8:1 OS, 2MiB + one 4MiB - bisect permutation
run_sim "perm_os8_2MiB_one4MiB" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_2MiB_one4MiB_bisect.cm" "-plb on -subflows 2" 5000

# ============================================================
echo ""
echo "=== Permutation with Packet Trimming (Fig 1 & Fig 8 overlay) ==="
# ============================================================

# Fig 1(a): 8:1 OS, 2MiB - with trimming
run_sim "perm_os8_2MiB_trim"  "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_2MiB_bisect.cm"  "-plb on -subflows 2 -trimming on" 3000

# Fig 1(b): 8:1 OS, 32MiB - with trimming
run_sim "perm_os8_32MiB_trim" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm" "-plb on -subflows 2 -trimming on" 8000

# Fig 8a: 2:1 OS, 32MiB - with trimming
run_sim "perm_os2_32MiB_trim" "$TOPO_OS2" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm" "-plb on -subflows 2 -trimming on" 5000

# Fig 8b: 4:1 OS, 32MiB - with trimming
run_sim "perm_os4_32MiB_trim" "$TOPO_OS4" "$CM_DIR/perm_1024n_1024c_32MiB_bisect.cm" "-plb on -subflows 2 -trimming on" 5000

# Fig 8c: 8:1 OS, 4x4MiB parallel permutations - with trimming
run_sim "perm_os8_4x4MiB_trim" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_4x4MiB_bisect.cm" "-plb on -subflows 2 -trimming on" 5000

# Fig 8d: 8:1 OS, 2MiB + one 4MiB - with trimming
run_sim "perm_os8_2MiB_one4MiB_trim" "$TOPO_OS8" "$CM_DIR/perm_1024n_1024c_2MiB_one4MiB_bisect.cm" "-plb on -subflows 2 -trimming on" 5000

# ============================================================
echo ""
echo "=== Section 4.2: Incast (Fig 5) ==="
# ============================================================

TOPO_NB="$TOPO_DIR/fat_tree_1024_800g_nb.topo"
SIZES=(4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432 67108864)

for DEGREE in 8 32 100; do
    for SIZE in "${SIZES[@]}"; do
        SIZEKIB=$((SIZE / 1024))
        CM="$CM_DIR/incast_1024n_${DEGREE}deg_${SIZEKIB}KiB.cm"
        # Endtime must account for degree: all flows share one bottleneck.
        # Use degree*size*8/BW * 20x margin + 5ms base, capped at 2000ms.
        # cwnd=1 slows initial send rate, so we need more generous margin.
        ENDTIME=$(python3 -c "print(max(5000, min(2000000, int($DEGREE * $SIZE * 8 / 800000000 * 1e6 * 20 + 5000))))") 
        run_sim "incast_${DEGREE}deg_${SIZEKIB}KiB" "$TOPO_NB" "$CM" "-plb on -subflows 1 -cwnd 1" "$ENDTIME"
    done
done

# ============================================================
echo ""
echo "=== Section 4.4: AllToAll (Fig 9) ==="
# ============================================================

TOPO_128_OS8="$TOPO_DIR/fat_tree_128_800g_os8.topo"

for K in 1 2 8 16; do
    CM="$CM_DIR/a2a_128n_1MiB_k${K}.cm"
    run_sim "a2a_128n_1MiB_k${K}" "$TOPO_128_OS8" "$CM" "-plb on -subflows 2" 20000
done

echo ""
echo "=== All Swift experiments done. Results in $OUT_DIR ==="
echo "Run parse_output to process .dat files."
