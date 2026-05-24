#!/bin/bash
# Generates all connection matrices needed for Swift vs FASTFLOW evaluation
# Paper: 1024 nodes, 800Gbps, 4KiB MTU, 400ns switch, 600ns link
# Run from sim/datacenter/connection_matrices/

set -e
cd "$(dirname "$0")"

RANDSEED=42

echo "=== Generating Permutation matrices ==="

# Bisect permutations: pod i -> pod (i+1)%16, guarantees NO intra-pod flows
# matching paper's definition of 'bisect permutation'
python3 - <<'PYEOF'
import random

def make_bisect_perm(nodes, hosts_per_pod, seed):
    random.seed(seed)
    pods = nodes // hosts_per_pod
    dst_pools = {p: list(range(p*hosts_per_pod, (p+1)*hosts_per_pod)) for p in range(pods)}
    for p in range(pods):
        random.shuffle(dst_pools[p])
    perm = [None] * nodes
    for p in range(pods):
        dst_pod = (p + 1) % pods
        for i, src in enumerate(range(p*hosts_per_pod, (p+1)*hosts_per_pod)):
            perm[src] = dst_pools[dst_pod][i]
    assert sorted(perm) == list(range(nodes))
    assert all(i//hosts_per_pod != perm[i]//hosts_per_pod for i in range(nodes))
    return perm

configs = [
    ('perm_1024n_1024c_2MiB_bisect.cm',          2097152,  [42]),
    ('perm_1024n_1024c_32MiB_bisect.cm',          33554432, [42]),
    ('perm_1024n_1024c_4MiB_bisect.cm',           4194304,  [42]),
    ('perm_1024n_1024c_2MiB_one4MiB_bisect.cm',   2097152,  [42]),  # first flow patched to 4MiB below
]

for fname, size, seeds in configs:
    all_conns = []
    for s in seeds:
        p = make_bisect_perm(1024, 64, s)
        for src in range(1024):
            all_conns.append((src, p[src], size))
    with open(fname, 'w') as f:
        f.write(f'Nodes 1024\nConnections {len(all_conns)}\n')
        for i, (src, dst, sz) in enumerate(all_conns, 1):
            f.write(f'{src}->{dst} id {i} start 0 size {sz}\n')
    print(f'  {fname} done')

# Patch 2MiB_one4MiB: change first flow to 4MiB
import re
lines = open('perm_1024n_1024c_2MiB_one4MiB_bisect.cm').readlines()
with open('perm_1024n_1024c_2MiB_one4MiB_bisect.cm', 'w') as f:
    first = True
    for l in lines:
        if first and re.match(r'^\d+', l):
            l = re.sub(r'size \d+', 'size 4194304', l, count=1)
            first = False
        f.write(l)
print('  perm_1024n_1024c_2MiB_one4MiB_bisect.cm patched (first flow 4MiB)')

# 4x4MiB: 4 bisect permutations concatenated
all_conns = []
for seed in [42, 43, 44, 45]:
    p = make_bisect_perm(1024, 64, seed)
    for src in range(1024):
        all_conns.append((src, p[src]))
with open('perm_1024n_1024c_4x4MiB_bisect.cm', 'w') as f:
    f.write(f'Nodes 1024\nConnections {len(all_conns)}\n')
    for i, (src, dst) in enumerate(all_conns, 1):
        f.write(f'{src}->{dst} id {i} start 0 size 4194304\n')
print(f'  perm_1024n_1024c_4x4MiB_bisect.cm done ({len(all_conns)} connections)')
PYEOF

# Keep old random permutations as backups (not used in main eval runs)
python3 gen_permutation.py perm_1024n_1024c_2MiB.cm 1024 1024 2097152 0 $RANDSEED
python3 gen_permutation.py perm_1024n_1024c_32MiB.cm 1024 1024 33554432 0 $RANDSEED
python3 gen_permutation.py perm_1024n_1024c_4MiB.cm 1024 1024 4194304 0 $RANDSEED

echo ""
echo "=== Generating Incast matrices ==="

# Fig 5: incast with fan-in degrees 8, 32, 100
# Message sizes: 2^2..2^16 KiB = 4KiB..64MiB (15 sizes)
SIZES=(4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432 67108864)

for DEGREE in 8 32 100; do
    for SIZE in "${SIZES[@]}"; do
        SIZEKIB=$((SIZE / 1024))
        FNAME="incast_1024n_${DEGREE}deg_${SIZEKIB}KiB.cm"
        python3 gen_incast.py "$FNAME" 1024 $DEGREE $SIZE 0 $RANDSEED
        echo "  $FNAME done"
    done
done

echo ""
echo "=== Generating AllToAll matrices (128 nodes, 1MiB, k=1,2,8,16) ==="
# Uses gen_serialn_alltoall.py: conns_per_group=128, groupsize=128, parallel=k
# 128 nodes, each node sends to 127 others = 128*(128-1) = 16256 total pairs
# We model as: 128 nodes in one big group, each sends to all 127 others, parallel=k

for K in 1 2 8 16; do
    FNAME="a2a_128n_1MiB_k${K}.cm"
    python3 gen_serialn_alltoall.py "$FNAME" 128 128 128 $K 1048576 0 $RANDSEED
    echo "  $FNAME done"
done

echo ""
echo "All matrices generated."
