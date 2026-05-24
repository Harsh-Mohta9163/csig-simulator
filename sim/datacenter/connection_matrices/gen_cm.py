#!/usr/bin/env python3
"""
gen_cm.py  --  Generate connection matrices for htsim Swift evaluation.

Usage examples:
  # Standard permutation, 1024 nodes, all 32MiB:
  python3 gen_cm.py --nodes 1024 --size 32MiB --pattern perm

  # Bisection permutation (all flows cross network midpoint):
  python3 gen_cm.py --nodes 1024 --size 32MiB --pattern bisect_perm

  # All 2MiB except one random flow is 4MiB:
  python3 gen_cm.py --nodes 1024 --size 2MiB --pattern bisect_perm --one-large 4MiB

  # Each node sends 4 flows (4x4MiB):
  python3 gen_cm.py --nodes 1024 --size 4MiB --pattern bisect_perm --flows-per-node 4

  # Custom output name:
  python3 gen_cm.py --nodes 1024 --size 2MiB --pattern bisect_perm -o my_matrix.cm

  # Mixed sizes: specify sizes as comma-separated list (cycles through flows):
  python3 gen_cm.py --nodes 1024 --pattern bisect_perm --mixed-sizes 2MiB,2MiB,2MiB,4MiB

  # Override output directory:
  python3 gen_cm.py --nodes 1024 --size 32MiB --pattern perm --outdir /tmp/

Supported patterns:
  perm          -- Random permutation (every node sends to one unique destination)
  bisect_perm   -- Bisection permutation (src in first half -> dst in second half, and vice versa)

Size format:  B, KiB, MiB, GiB  (e.g.  4096B, 64KiB, 2MiB, 32MiB)
             or raw bytes (e.g. 33554432)
"""

import argparse
import os
import random
import sys


# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

def parse_size(s):
    """Convert human-readable size string to bytes.  e.g. '32MiB' -> 33554432"""
    s = s.strip()
    units = {"B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3,
             "KB": 1000, "MB": 1000**2, "GB": 1000**3}
    for suffix, mult in sorted(units.items(), key=lambda x: -len(x[0])):
        if s.endswith(suffix):
            val = float(s[:-len(suffix)])
            return int(val * mult)
    return int(s)  # raw bytes


def size_label(n_bytes):
    """Return a concise label like '2MiB', '32MiB' for a byte count."""
    for unit, mult in [("GiB", 1024**3), ("MiB", 1024**2), ("KiB", 1024)]:
        if n_bytes % mult == 0:
            return f"{n_bytes // mult}{unit}"
    return f"{n_bytes}B"


def auto_filename(nodes, pattern, size_bytes, flows_per_node, one_large_bytes, mixed_sizes, outdir):
    """Build a descriptive filename matching the project naming convention."""
    conns = nodes * flows_per_node

    if mixed_sizes:
        # e.g. 2MiB_one4MiB or 4x4MiB
        counts = {}
        for s in mixed_sizes:
            lbl = size_label(s)
            counts[lbl] = counts.get(lbl, 0) + 1
        if len(counts) == 1:
            lbl, cnt = list(counts.items())[0]
            size_part = f"{cnt}x{lbl}" if cnt > 1 else lbl
        else:
            # e.g. 2MiB_one4MiB: dominant size first
            dominant = max(counts, key=lambda k: counts[k])
            others = {k: v for k, v in counts.items() if k != dominant}
            parts = [dominant] + [f"one{k}" if v == 1 else f"{v}x{k}" for k, v in others.items()]
            size_part = "_".join(parts)
    elif one_large_bytes:
        base_lbl  = size_label(size_bytes)
        large_lbl = size_label(one_large_bytes)
        size_part = f"{base_lbl}_one{large_lbl}"
    else:
        size_part = size_label(size_bytes)
        if flows_per_node > 1:
            size_part = f"{flows_per_node}x{size_part}"

    suffix = "bisect" if pattern == "bisect_perm" else ""
    name_parts = [f"perm_{nodes}n_{conns}c_{size_part}"]
    if suffix:
        name_parts.append(suffix)
    filename = "_".join(name_parts) + ".cm"
    return os.path.join(outdir, filename)


# ──────────────────────────────────────────────────────────────────────────────
# Permutation generators
# ──────────────────────────────────────────────────────────────────────────────

def gen_perm(nodes, rng):
    """
    Standard random permutation: shuffle destinations so no src sends to itself.
    Returns list of (src, dst) pairs, length = nodes.
    """
    dsts = list(range(nodes))
    rng.shuffle(dsts)

    # Fix self-loops by swapping with the next element
    for i in range(nodes):
        if dsts[i] == i:
            j = (i + 1) % nodes
            dsts[i], dsts[j] = dsts[j], dsts[i]

    # Final pass: catch any remaining (shouldn't happen but be safe)
    for i in range(nodes):
        if dsts[i] == i:
            # find any j != i where dsts[j] != i as well
            for j in range(nodes):
                if j != i and dsts[j] != i:
                    dsts[i], dsts[j] = dsts[j], dsts[i]
                    break

    return list(zip(range(nodes), dsts))


def gen_bisect_perm(nodes, rng):
    """
    Bisection permutation: every flow crosses the network midpoint.
      - Nodes 0 .. N/2-1  (first half)  send to  N/2 .. N-1  (second half)
      - Nodes N/2 .. N-1  (second half) send to  0   .. N/2-1 (first half)

    This is the worst-case traffic for the spine/core layer.
    """
    half = nodes // 2
    first_half  = list(range(0, half))
    second_half = list(range(half, nodes))

    # Shuffle each half independently to create a random cross-bisection mapping
    rng.shuffle(first_half)
    rng.shuffle(second_half)

    pairs = []
    # first half i -> second half i  (and vice versa)
    for i in range(half):
        src_a, dst_a = first_half[i],  second_half[i]   # first->second
        src_b, dst_b = second_half[i], first_half[i]    # second->first
        pairs.append((src_a, dst_a))
        pairs.append((src_b, dst_b))

    # Sort by src so the file is readable
    pairs.sort(key=lambda x: x[0])
    return pairs


def extend_to_k_flows(base_pairs, k, nodes, rng):
    """
    Given a base permutation (1 flow per node), create k independent permutations
    and combine them so each node sends k flows.
    Returns list of (src, dst) pairs, length = nodes * k.
    """
    if k == 1:
        return base_pairs

    # Determine which generator to use based on base_pairs pattern
    is_bisect = all(
        (s < nodes // 2 and d >= nodes // 2) or (s >= nodes // 2 and d < nodes // 2)
        for s, d in base_pairs
    )

    all_pairs = list(base_pairs)
    for _ in range(k - 1):
        if is_bisect:
            new_pairs = gen_bisect_perm(nodes, rng)
        else:
            new_pairs = gen_perm(nodes, rng)
        all_pairs.extend(new_pairs)

    # Sort by src then dst for readability
    all_pairs.sort(key=lambda x: (x[0], x[1]))
    return all_pairs


# ──────────────────────────────────────────────────────────────────────────────
# File writer
# ──────────────────────────────────────────────────────────────────────────────

def write_cm(path, nodes, pairs, sizes):
    """
    Write the connection matrix file.

    pairs  -- list of (src, dst)
    sizes  -- list of flow sizes in bytes, same length as pairs
               OR a single int (same size for all)
    """
    if isinstance(sizes, int):
        sizes = [sizes] * len(pairs)

    assert len(pairs) == len(sizes), "pairs and sizes must have the same length"

    with open(path, "w") as f:
        f.write(f"Nodes {nodes}\n")
        f.write(f"Connections {len(pairs)}\n")
        for flow_id, ((src, dst), size) in enumerate(zip(pairs, sizes), start=1):
            f.write(f"{src}->{dst} id {flow_id} start 0 size {size}\n")

    print(f"Written {len(pairs)} connections -> {path}")


# ──────────────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────────────

def main():
    default_outdir = os.path.join(os.path.dirname(__file__))

    ap = argparse.ArgumentParser(
        description="Generate htsim connection matrix (.cm) files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    # Core options
    ap.add_argument("--nodes", type=int, default=1024,
                    help="Number of nodes in topology (default: 1024)")
    ap.add_argument("--pattern", choices=["perm", "bisect_perm"], default="bisect_perm",
                    help="Traffic pattern: perm | bisect_perm (default: bisect_perm)")
    ap.add_argument("--seed", type=int, default=42,
                    help="Random seed (default: 42, use 0 for non-deterministic)")

    # Size options (mutually exclusive group)
    size_group = ap.add_mutually_exclusive_group()
    size_group.add_argument("--size", type=str, default=None,
                    help="Flow size, e.g. 2MiB, 32MiB, 64KiB (all flows same size)")
    size_group.add_argument("--mixed-sizes", type=str, default=None,
                    help="Comma-separated sizes cycling through flows, e.g. 2MiB,2MiB,2MiB,4MiB. "
                         "Total connections = nodes * ceil(len/nodes) rounded to full permutations.")

    # Multi-flow options
    ap.add_argument("--flows-per-node", type=int, default=1,
                    help="Number of flows per source node (default: 1). "
                         "e.g. --flows-per-node 4 for 4x4MiB")
    ap.add_argument("--one-large", type=str, default=None,
                    help="Make exactly ONE randomly chosen flow this size instead of --size. "
                         "e.g. --size 2MiB --one-large 4MiB  ->  1023 flows of 2MiB + 1 of 4MiB")

    # Output
    ap.add_argument("-o", "--output", type=str, default=None,
                    help="Output filename (auto-generated if not given)")
    ap.add_argument("--outdir", type=str, default=default_outdir,
                    help=f"Output directory (default: {default_outdir})")

    # Batch mode
    ap.add_argument("--batch", action="store_true",
                    help="Generate a standard batch of common matrices (ignores most other flags)")

    args = ap.parse_args()

    rng = random.Random(args.seed if args.seed != 0 else None)

    os.makedirs(args.outdir, exist_ok=True)

    # ── BATCH MODE ────────────────────────────────────────────────────────────
    if args.batch:
        batch_generate(args.nodes, args.outdir, rng)
        return

    # ── SINGLE MATRIX MODE ───────────────────────────────────────────────────
    nodes = args.nodes

    # Validate sizes
    if args.mixed_sizes is None and args.size is None:
        ap.error("Provide --size or --mixed-sizes (or --batch)")

    # Build base permutation (1 flow per node)
    if args.pattern == "bisect_perm":
        if nodes % 2 != 0:
            ap.error("bisect_perm requires an even number of nodes")
        base_pairs = gen_bisect_perm(nodes, rng)
    else:
        base_pairs = gen_perm(nodes, rng)

    # Extend to k flows per node if requested
    k = args.flows_per_node
    if args.mixed_sizes:
        mixed = [parse_size(s.strip()) for s in args.mixed_sizes.split(",")]
        k = max(k, len(mixed))  # need at least as many perms as distinct size slots

    pairs = extend_to_k_flows(base_pairs, k, nodes, rng)

    # Build sizes list
    if args.mixed_sizes:
        mixed = [parse_size(s.strip()) for s in args.mixed_sizes.split(",")]
        sizes = [mixed[i % len(mixed)] for i in range(len(pairs))]
        one_large_bytes = None
    else:
        base_size = parse_size(args.size)
        one_large_bytes = parse_size(args.one_large) if args.one_large else None
        if one_large_bytes:
            sizes = [base_size] * len(pairs)
            large_idx = rng.randint(0, len(pairs) - 1)
            sizes[large_idx] = one_large_bytes
            print(f"Large flow: id={large_idx+1}  "
                  f"{pairs[large_idx][0]}->{pairs[large_idx][1]}  "
                  f"size={size_label(one_large_bytes)}")
        else:
            sizes = base_size

    # Determine output path
    if args.output:
        out_path = args.output if os.path.isabs(args.output) else os.path.join(args.outdir, args.output)
    else:
        out_path = auto_filename(
            nodes, args.pattern,
            parse_size(args.size) if args.size else parse_size(args.mixed_sizes.split(",")[0]),
            k,
            one_large_bytes if not args.mixed_sizes else None,
            [parse_size(s.strip()) for s in args.mixed_sizes.split(",")] if args.mixed_sizes else None,
            args.outdir
        )

    write_cm(out_path, nodes, pairs, sizes)


# ──────────────────────────────────────────────────────────────────────────────
# Batch generator  (reproduces the standard eval set)
# ──────────────────────────────────────────────────────────────────────────────

def batch_generate(nodes, outdir, rng):
    """Generate the standard set of connection matrices used in run_trimming_perm.sh"""
    print(f"=== Batch generating matrices for {nodes} nodes -> {outdir} ===\n")

    def make(pattern, size_bytes, k=1, one_large=None, mixed=None, seed_offset=0):
        local_rng = random.Random(rng.randint(0, 2**32) + seed_offset)
        if pattern == "bisect_perm":
            base = gen_bisect_perm(nodes, local_rng)
        else:
            base = gen_perm(nodes, local_rng)

        pairs = extend_to_k_flows(base, k, nodes, local_rng)

        if mixed:
            sizes = [mixed[i % len(mixed)] for i in range(len(pairs))]
            one_large = None
        elif one_large:
            sizes = [size_bytes] * len(pairs)
            idx = local_rng.randint(0, len(pairs) - 1)
            sizes[idx] = one_large
        else:
            sizes = size_bytes

        path = auto_filename(nodes, pattern, size_bytes, k, one_large, mixed, outdir)
        write_cm(path, nodes, pairs, sizes)

    MiB = 1024 * 1024

    # ── Bisection permutation variants ──────────────────────────────────────
    print("-- bisect_perm --")
    make("bisect_perm", 2*MiB)                               # perm_Nn_Nc_2MiB_bisect.cm
    make("bisect_perm", 4*MiB)                               # perm_Nn_Nc_4MiB_bisect.cm
    make("bisect_perm", 32*MiB)                              # perm_Nn_Nc_32MiB_bisect.cm
    make("bisect_perm", 4*MiB,  k=4)                         # perm_Nn_Nc_4x4MiB_bisect.cm
    make("bisect_perm", 2*MiB,  one_large=4*MiB)             # perm_Nn_Nc_2MiB_one4MiB_bisect.cm

    # ── Standard (non-bisect) permutation variants ───────────────────────────
    print("\n-- perm --")
    make("perm", 2*MiB)                                      # perm_Nn_Nc_2MiB.cm
    make("perm", 4*MiB)                                      # perm_Nn_Nc_4MiB.cm
    make("perm", 32*MiB)                                     # perm_Nn_Nc_32MiB.cm
    make("perm", 4*MiB, k=4)                                 # perm_Nn_Nc_4x4MiB.cm
    make("perm", 2*MiB, one_large=4*MiB)                     # perm_Nn_Nc_2MiB_one4MiB.cm

    print("\nDone.")


if __name__ == "__main__":
    main()
