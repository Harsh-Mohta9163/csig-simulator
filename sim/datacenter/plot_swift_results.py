#!/usr/bin/env python3
"""
plot_swift_results.py  --  Parse .fct files from run_swift_eval.sh and
reproduce the key figures from the FASTFLOW paper (Swift curve only).

Usage:
    python3 plot_swift_results.py [--results results/swift] [--out plots/]

Output files (all PDF + PNG):
    fig5_incast_normalized.pdf   -- Fig 5 equivalent (normalized throughput vs message size)
    fig8_permutation_cdf.pdf     -- Fig 8 equivalent (FCT CDF for permutations)
    fig9_alltoall_cdf.pdf        -- Fig 9 equivalent (FCT CDF for AllToAll, k=1,2,8,16)
"""

import os, sys, re, argparse
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ────────────────────────────────────────────────────────────────
LINK_GBPS = 800.0          # 800 Gbps NIC / link speed
LINK_BPS  = LINK_GBPS * 1e9

# Minimum RTT for 1024-node 3-tier fat tree (600ns link, 400ns switch):
#   Cross-pod path: 6 links * 600ns + 5 switches * 400ns = 5600ns one-way
#   RTT = 11200ns = 11.2us
# This MUST be added to the theoretical best time — propagation is unavoidable.
# Without it, small messages look terrible because theory≪RTT even when perf is optimal.
MIN_RTT_US = 2 * (6 * 600 + 5 * 400) / 1000   # 11.2 us

SWIFT_COLOR   = "#9467bd"    # purple, matching paper
SWIFT_LABEL   = "Swift"
TRIM_COLOR    = "#17becf"    # teal for trimming variant
TRIM_LABEL    = "Swift"

# ────────────────────────────────────────────────────────────────
def parse_fct_file(path):
    """Return list of dicts with keys: name, start_us, finish_us, fct_us, size_bytes."""
    rows = []
    try:
        with open(path) as f:
            for line in f:
                m = re.match(
                    r"FCT\s+(\S+)\s+start_us\s+([\d.]+)\s+finish_us\s+([\d.]+)"
                    r"\s+fct_us\s+([\d.]+)\s+size_bytes\s+(\d+)",
                    line.strip()
                )
                if m:
                    rows.append({
                        "name":       m.group(1),
                        "start_us":   float(m.group(2)),
                        "finish_us":  float(m.group(3)),
                        "fct_us":     float(m.group(4)),
                        "size_bytes": int(m.group(5)),
                    })
    except FileNotFoundError:
        pass
    return rows


def fcts(rows):
    return np.array([r["fct_us"] for r in rows])


def incast_endtime_us(size_bytes, degree):
    """Compute the simulation endtime (us) used for a given incast flow size + degree.
    Must match the formula in run_swift_eval.sh."""
    return max(5000, min(2000000, int(degree * size_bytes * 8 / LINK_BPS * 1e6 * 20 + 5000)))


def normalized_throughput_incast(rows, degree, size_bytes):
    """Incast normalized throughput = mean over all flows of (theory_fct / actual_fct).

    For each individual flow:
      theory_fct_i = (degree * size_bytes * 8 / link_bps) + MIN_RTT
        = bottleneck transmission time + propagation delay
      actual_fct_i = observed FCT (or endtime if flow never completed)

    Uses per-flow average so that partial completions produce proportional
    degradation rather than collapsing to near-zero from a single slow flow.

    MIN_RTT correction prevents small messages (RTT >> tx_time) from appearing
    terrible when their actual FCT is dominated by propagation, not congestion.
    """
    tx_us     = degree * size_bytes * 8 / LINK_BPS * 1e6   # bottleneck tx time
    theory_us = tx_us + MIN_RTT_US                          # includes propagation

    endtime   = incast_endtime_us(size_bytes, degree)
    n_complete = len(rows)
    n_total    = degree

    if n_complete == 0:
        # No completions: all flows get the endtime penalty
        return min(1.0, theory_us / endtime)

    # Per-flow average: completed flows use their actual FCT;
    # incomplete flows use the simulation endtime as a conservative penalty.
    completed_fcts = [r["fct_us"] for r in rows]
    total_norm = sum(min(1.0, theory_us / fct) for fct in completed_fcts)
    # Incomplete flows: normalized = theory / endtime
    n_incomplete = n_total - n_complete
    total_norm += n_incomplete * min(1.0, theory_us / endtime)

    return total_norm / n_total


def cdf(data):
    s = np.sort(data)
    y = np.arange(1, len(s) + 1) / len(s)
    return s, y


def overall_and_spread(rows):
    """Returns (overall_fct_us, spread_us) matching paper annotation format:
    first number = time when last flow finishes, second = max - min FCT."""
    v = fcts(rows)
    if len(v) == 0:
        return float("nan"), float("nan")
    return np.max(v), np.max(v) - np.min(v)


# ────────────────────────────────────────────────────────────────
# Fig 5 — Incast: Normalized throughput vs message size
# ────────────────────────────────────────────────────────────────
def plot_fig5_incast(results_dir, out_dir):
    SIZES_KiB = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
    DEGREES   = [8, 32, 100]
    SUB_TITLES = ["8:1 Incast", "32:1 Incast", "100:1 Incast"]

    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5), sharey=True)
    fig.suptitle("Message Scaling During Incast\n1024 Nodes – 800Gbps – 4KiB MTU", fontsize=11)

    for ax, degree, subtitle in zip(axes, DEGREES, SUB_TITLES):
        xs, ys_norm = [], []
        for siz in SIZES_KiB:
            fname = results_dir / f"incast_{degree}deg_{siz}KiB.fct"
            rows  = parse_fct_file(fname)
            if not rows:
                # No completions at all — use endtime
                nt = normalized_throughput_incast([], degree, siz * 1024)
            else:
                nt = normalized_throughput_incast(rows, degree, siz * 1024)
            xs.append(siz)
            ys_norm.append(nt)

        if xs:
            ax.plot(xs, ys_norm, marker="o", color=SWIFT_COLOR, label=SWIFT_LABEL,
                    linewidth=2, markersize=5)

        ax.set_xscale("log", base=2)
        ax.set_xlim(4, 65536)
        ax.set_ylim(0, 1.05)
        ax.set_xlabel("Message Size (KiB)")
        ax.set_title(f"Message Scaling During {degree}:1 Incast\n"
                     f"1024 Nodes – 800Gbps – 4KiB MTU", fontsize=9)
        ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    axes[0].set_ylabel("Normalized to Theo. Best")
    plt.tight_layout()
    _save(fig, out_dir, "fig5_incast_normalized")
    print("Saved fig5_incast_normalized")


# ────────────────────────────────────────────────────────────────
# Fig 8 — Permutation: CDF of FCT for different OS ratios / flow mixes
# ────────────────────────────────────────────────────────────────
def plot_fig8_permutation(results_dir, out_dir):
    configs = [
        ("perm_os2_32MiB",         "Bisect Permutation 2:1 OS – 32MiB\n1024 Nodes – 800Gbps – 4KiB MTU"),
        ("perm_os4_32MiB",         "Bisect Permutation 4:1 OS – 32MiB\n1024 Nodes – 800Gbps – 4KiB MTU"),
        ("perm_os8_4x4MiB",        "Bisect Perm. 8:1 OS – 4×4MiB\n1024 Nodes – 800Gbps – 4KiB MTU"),
        ("perm_os8_2MiB_one4MiB",  "Bisect Perm. 8:1 OS – 2MiB (one 4MiB)\n1024 Nodes – 800Gbps – 4KiB MTU"),
    ]

    fig, axes = plt.subplots(1, 4, figsize=(20, 4.5))
    fig.suptitle("Flow Completion Time – Permutation (Swift)", fontsize=11)

    for ax, (label, title) in zip(axes, configs):
        # Swift + Trimming only
        fname_trim = results_dir / f"{label}_trim.fct"
        rows_trim  = parse_fct_file(fname_trim)
        if rows_trim:
            v, y = cdf(fcts(rows_trim))
            overall, spread = overall_and_spread(rows_trim)
            ax.plot(v, y * 100, color=TRIM_COLOR, linewidth=2,
                    label=f"{TRIM_LABEL}\n{overall:.0f}us, {spread:.0f}us")
        else:
            ax.text(0.5, 0.5, "No data\n(run eval first)",
                    ha="center", va="center", transform=ax.transAxes, fontsize=9, color="gray")

        ax.legend(fontsize=7, loc="lower right")
        ax.set_xlabel("Flow Completion Time (μs)")
        ax.set_ylabel("CDF (%)")
        ax.set_title(title, fontsize=8)
        ax.set_ylim(0, 100)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    _save(fig, out_dir, "fig8_permutation_cdf")
    print("Saved fig8_permutation_cdf")


# ────────────────────────────────────────────────────────────────
# Fig 1 — Permutation: CDF of FCT for 8:1 OS, 2MiB and 32MiB
# ────────────────────────────────────────────────────────────────
def plot_fig1_permutation(results_dir, out_dir):
    configs = [
        ("perm_os8_2MiB",  "Bisect Permutation 8:1 OS – 2MiB\n1024 Nodes – 800Gbps – 4KiB MTU"),
        ("perm_os8_32MiB", "Bisect Permutation 8:1 OS – 32MiB\n1024 Nodes – 800Gbps – 4KiB MTU"),
    ]

    fig, axes = plt.subplots(1, 2, figsize=(10, 4.5))
    fig.suptitle("Flow Completion Time – Bisect Permutation 8:1 OS (Swift)", fontsize=11)

    for ax, (label, title) in zip(axes, configs):
        # Swift + Trimming only
        fname_trim = results_dir / f"{label}_trim.fct"
        rows_trim  = parse_fct_file(fname_trim)
        if rows_trim:
            size_bytes = rows_trim[0]["size_bytes"]
            ideal_us = size_bytes * 8 / LINK_BPS * 1e6
            ax.axvline(ideal_us, color="olive", linestyle="--", linewidth=1.2,
                       label=f"Ideal ({ideal_us:.0f}\u03bcs)")
            v, y = cdf(fcts(rows_trim))
            overall, spread = overall_and_spread(rows_trim)
            ax.plot(v, y * 100, color=TRIM_COLOR, linewidth=2,
                    label=f"{TRIM_LABEL}\n{overall:.0f}us, {spread:.0f}us")
        else:
            ax.text(0.5, 0.5, "No data\n(run eval first)",
                    ha="center", va="center", transform=ax.transAxes, fontsize=9, color="gray")

        ax.legend(fontsize=8, loc="lower right")
        ax.set_xlabel("Flow Completion Time (μs)")
        ax.set_ylabel("CDF (%)")
        ax.set_title(title, fontsize=9)
        ax.set_ylim(0, 100)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    _save(fig, out_dir, "fig1_permutation_cdf")
    print("Saved fig1_permutation_cdf")


# ────────────────────────────────────────────────────────────────
# Fig 9 — AllToAll: CDF of FCT, windowed k=1,2,8,16
# ────────────────────────────────────────────────────────────────
def plot_fig9_alltoall(results_dir, out_dir):
    KS = [1, 2, 8, 16]
    COLORS = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728"]   # one color per k

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.set_title("AllToAll 128 Nodes – 8:1 OS – 1MiB – 800Gbps – 4KiB MTU\n(Swift)", fontsize=10)

    for k, color in zip(KS, COLORS):
        fname = results_dir / f"a2a_128n_1MiB_k{k}.fct"
        rows  = parse_fct_file(fname)
        if rows:
            v, y = cdf(fcts(rows))
            overall, spread = overall_and_spread(rows)
            ax.plot(v, y * 100, color=color, linewidth=2,
                    label=f"k={k}  ({overall:.0f}\u03bcs, spread={spread:.0f}\u03bcs)")

    ax.set_xlabel("Flow Completion Time (μs)")
    ax.set_ylabel("CDF (%)")
    ax.set_ylim(0, 100)
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    _save(fig, out_dir, "fig9_alltoall_cdf")
    print("Saved fig9_alltoall_cdf")


# ────────────────────────────────────────────────────────────────
def _save(fig, out_dir, name):
    out_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(out_dir / f"{name}.{ext}", dpi=150, bbox_inches="tight")
    plt.close(fig)


# ────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results/swift",
                    help="Directory containing .fct files (default: results/swift)")
    ap.add_argument("--out", default="plots",
                    help="Output directory for plots (default: plots)")
    args = ap.parse_args()

    results_dir = Path(args.results)
    out_dir     = Path(args.out)

    if not results_dir.exists():
        print(f"Results directory '{results_dir}' not found.")
        print("Run:  bash run_swift_eval.sh   first.")
        sys.exit(1)

    print(f"Reading .fct files from: {results_dir}")
    print(f"Writing plots to:        {out_dir}")
    print()

    plot_fig1_permutation(results_dir, out_dir)
    plot_fig8_permutation(results_dir, out_dir)

    print()
    print(f"All plots written to {out_dir}/")


if __name__ == "__main__":
    main()
