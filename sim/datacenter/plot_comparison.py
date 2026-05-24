#!/usr/bin/env python3
"""
plot_comparison.py — Generate paper figures comparing FASTFLOW variants vs Swift.

Reads from:
  results/swift/          -- Swift baseline
  results/fastflow/<tag>/ -- FASTFLOW variants (tag = mode string with + replaced by _)

Output figures (PDF + PNG in plots/):
  fig1_perm_2MiB_cdf.pdf       -- Fig 1: Permutation 2MiB CDF — all protocols
  fig5_incast_normalized.pdf   -- Fig 5: Incast normalized throughput vs message size
  fig_raqaablation.pdf         -- RA-QA ablation on 32-deg and 100-deg incast
  fig_mcc_ablation.pdf         -- MCC ablation: FF vs FF+EQDS vs FF+EQDS+MCC
  fig_incast_cdf.pdf           -- FCT CDF at key incast sizes (32-deg and 100-deg)

Usage:
    cd sim/datacenter
    python3 plot_comparison.py [--out plots]
"""

import os, re, sys, argparse
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ──────────────────────────────────────────────────────────────────────────────
# Constants
# ──────────────────────────────────────────────────────────────────────────────
LINK_GBPS = 800.0
LINK_BPS  = LINK_GBPS * 1e9
# Minimum RTT: 6 links × 600ns + 5 switches × 400ns one-way = 5600ns → 11.2us RTT
MIN_RTT_US = 2 * (6 * 600 + 5 * 400) / 1000  # 11.2 us

# Protocol color palette (paper-friendly)
PROTO_STYLES = {
    "swift":                   {"color": "#9467bd", "ls": "-",  "lw": 2.0, "label": "Swift"},
    "fastflow":                {"color": "#1f77b4", "ls": "-",  "lw": 2.0, "label": "FASTFLOW"},
    "fastflow+eqds":           {"color": "#ff7f0e", "ls": "--", "lw": 2.0, "label": "FASTFLOW+EQDS"},
    "fastflow+eqds+mcc":       {"color": "#2ca02c", "ls": "-.", "lw": 2.0, "label": "FASTFLOW+EQDS+MCC"},
    "fastflow+ra_qa":          {"color": "#17becf", "ls": ":",  "lw": 2.0, "label": "FASTFLOW+RA-QA"},
    "fastflow+eqds+mcc+ra_qa": {"color": "#d62728", "ls": ":", "lw": 2.0, "label": "FF+EQDS+MCC+RA-QA"},
}


# ──────────────────────────────────────────────────────────────────────────────
# Data loading
# ──────────────────────────────────────────────────────────────────────────────
def parse_fct_file(path):
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


def cdf(data):
    s = np.sort(data)
    y = np.arange(1, len(s) + 1) / len(s)
    return s, y


def percentile(data, p):
    if len(data) == 0:
        return float("nan")
    return np.percentile(data, p)


def normalized_throughput_incast(rows, degree, size_bytes):
    tx_us     = degree * size_bytes * 8 / LINK_BPS * 1e6
    theory_us = tx_us + MIN_RTT_US
    endtime   = max(5000, min(2000000, int(degree * size_bytes * 8 / 800000000 * 1e6 * 20 + 5000)))
    if not rows:
        return min(1.0, theory_us / endtime)
    total_norm = sum(min(1.0, theory_us / r["fct_us"]) for r in rows)
    n_incomplete = degree - len(rows)
    total_norm += n_incomplete * min(1.0, theory_us / endtime)
    return total_norm / degree


def _results_dir_for_proto(results_root, proto):
    """Map protocol string → directory containing .fct files."""
    if proto == "swift":
        return results_root / "swift"
    # FASTFLOW tags: + → _, e.g. fastflow+eqds+mcc → fastflow_eqds_mcc
    # But ra_qa suffix is already added as +ra_qa in tag names
    tag = proto.replace("+", "_")
    return results_root / "fastflow" / tag


def load_incast(results_root, proto, degree, sizekib):
    d = _results_dir_for_proto(results_root, proto)
    # Swift uses same naming scheme
    f = d / f"incast_{degree}deg_{sizekib}KiB.fct"
    return parse_fct_file(f)


def load_perm(results_root, proto, label):
    d = _results_dir_for_proto(results_root, proto)
    f = d / f"{label}.fct"
    return parse_fct_file(f)


# ──────────────────────────────────────────────────────────────────────────────
# Fig 1 — Permutation 2MiB CDF: all protocols
# ──────────────────────────────────────────────────────────────────────────────
def plot_fig1_perm_cdf(results_root, out_dir):
    PROTOS = ["swift", "fastflow", "fastflow+eqds", "fastflow+eqds+mcc",
              "fastflow+eqds+mcc+ra_qa"]

    # Swift uses _trim suffix variant
    def _load(proto):
        if proto == "swift":
            return load_perm(results_root, proto, "perm_os8_2MiB_trim")
        return load_perm(results_root, proto, "perm_os8_2MiB")

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.set_title("Bisect Permutation – 2MiB – 1024 Nodes – 8:1 OS – 800Gbps", fontsize=10)

    # Ideal line
    ideal_us = 2 * 1024 * 1024 * 8 / LINK_BPS * 1e6
    ax.axvline(ideal_us, color="black", linestyle="--", linewidth=1.0, label=f"Ideal ({ideal_us:.0f}μs)")

    for proto in PROTOS:
        rows = _load(proto)
        if not rows:
            continue
        st = PROTO_STYLES.get(proto, {"color": "gray", "ls": "-", "lw": 1.5, "label": proto})
        v, y = cdf(fcts(rows))
        p50  = percentile(fcts(rows), 50)
        p99  = percentile(fcts(rows), 99)
        mx   = np.max(fcts(rows))
        ax.plot(v, y * 100, color=st["color"], linestyle=st["ls"],
                linewidth=st["lw"],
                label=f"{st['label']}  p50={p50:.0f}μs  max={mx:.0f}μs")

    ax.set_xlabel("Flow Completion Time (μs)")
    ax.set_ylabel("CDF (%)")
    ax.set_ylim(0, 100)
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.legend(fontsize=8, loc="lower right")
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    _save(fig, out_dir, "fig1_perm_2MiB_cdf")


# ──────────────────────────────────────────────────────────────────────────────
# Fig 5 — Incast normalized throughput vs message size
# ──────────────────────────────────────────────────────────────────────────────
def plot_fig5_incast(results_root, out_dir):
    PROTOS  = ["swift", "fastflow", "fastflow+eqds", "fastflow+eqds+mcc"]
    SIZES_KiB = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
    DEGREES = [8, 32, 100]
    SUBTITLES = ["8:1 Incast", "32:1 Incast", "100:1 Incast"]

    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5), sharey=True)
    fig.suptitle("Message Scaling During Incast – 1024 Nodes – 800Gbps – 4KiB MTU", fontsize=11)

    for ax, degree, subtitle in zip(axes, DEGREES, SUBTITLES):
        for proto in PROTOS:
            st = PROTO_STYLES.get(proto, {"color": "gray", "ls": "-", "lw": 1.5, "label": proto})
            xs, ys = [], []
            any_data = False
            for siz in SIZES_KiB:
                rows = load_incast(results_root, proto, degree, siz)
                nt = normalized_throughput_incast(rows, degree, siz * 1024)
                xs.append(siz)
                ys.append(nt)
                if rows:
                    any_data = True
            if any_data:
                ax.plot(xs, ys, marker="o", markersize=4,
                        color=st["color"], linestyle=st["ls"], linewidth=st["lw"],
                        label=st["label"])

        ax.set_xscale("log", base=2)
        ax.set_xlim(4, 65536)
        ax.set_ylim(0, 1.05)
        ax.set_xlabel("Message Size (KiB)")
        ax.set_title(f"{subtitle}\n1024 Nodes – 800Gbps", fontsize=9)
        ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    axes[0].set_ylabel("Normalized to Theo. Best")
    plt.tight_layout()
    _save(fig, out_dir, "fig5_incast_normalized")


# ──────────────────────────────────────────────────────────────────────────────
# RA-QA Ablation — FCT CDF at 32-deg and 100-deg, key sizes
# ──────────────────────────────────────────────────────────────────────────────
def plot_raqa_ablation(results_root, out_dir):
    CONFIGS = [
        (32,  128,   "32-deg 128KiB"),
        (32,  1024,  "32-deg 1024KiB"),
        (32,  4096,  "32-deg 4096KiB"),
        (100, 1024,  "100-deg 1024KiB"),
        (100, 4096,  "100-deg 4096KiB"),
    ]
    PROTO_PAIRS = [
        ("fastflow",          "fastflow+ra_qa",           "FASTFLOW"),
        ("fastflow+eqds+mcc", "fastflow+eqds+mcc+ra_qa",  "FF+EQDS+MCC"),
    ]

    fig, axes = plt.subplots(len(PROTO_PAIRS), len(CONFIGS),
                              figsize=(5 * len(CONFIGS), 4 * len(PROTO_PAIRS)))
    if len(PROTO_PAIRS) == 1:
        axes = [axes]

    fig.suptitle("RA-QA Ablation – Incast FCT CDF\n1024 Nodes – 800Gbps", fontsize=11)

    for row_idx, (base_proto, raqaproto, row_label) in enumerate(PROTO_PAIRS):
        for col_idx, (deg, siz, col_label) in enumerate(CONFIGS):
            ax = axes[row_idx][col_idx]
            for proto, suffix, color, ls in [
                (base_proto,  "",       "#1f77b4", "-"),
                (raqaproto,   "+RA-QA", "#d62728", "--"),
            ]:
                rows = load_incast(results_root, proto, deg, siz)
                if rows:
                    v, y = cdf(fcts(rows))
                    p50 = percentile(fcts(rows), 50)
                    mx  = np.max(fcts(rows))
                    ax.plot(v, y * 100, color=color, linestyle=ls, linewidth=1.8,
                            label=f"{row_label}{suffix}  p50={p50:.0f}  max={mx:.0f}")
            ax.set_title(f"{col_label}", fontsize=8)
            ax.set_xlabel("FCT (μs)", fontsize=8)
            ax.set_ylabel("CDF (%)", fontsize=8)
            ax.set_ylim(0, 100)
            ax.legend(fontsize=7)
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    _save(fig, out_dir, "fig_raqa_ablation")


# ──────────────────────────────────────────────────────────────────────────────
# MCC Ablation — FF vs FF+EQDS vs FF+EQDS+MCC
# ──────────────────────────────────────────────────────────────────────────────
def plot_mcc_ablation(results_root, out_dir):
    PROTOS = ["fastflow", "fastflow+eqds", "fastflow+eqds+mcc"]
    CONFIGS = [
        (8,   128,   "8-deg 128KiB"),
        (8,   4096,  "8-deg 4096KiB"),
        (32,  1024,  "32-deg 1024KiB"),
        (100, 1024,  "100-deg 1024KiB"),
        (100, 4096,  "100-deg 4096KiB"),
    ]
    COLORS = ["#1f77b4", "#ff7f0e", "#2ca02c"]

    fig, axes = plt.subplots(1, len(CONFIGS), figsize=(5 * len(CONFIGS), 4.5))
    fig.suptitle("FASTFLOW → EQDS → MCC Ablation – Incast\n1024 Nodes – 800Gbps", fontsize=11)

    for ax, (deg, siz, label) in zip(axes, CONFIGS):
        for proto, color in zip(PROTOS, COLORS):
            rows = load_incast(results_root, proto, deg, siz)
            if rows:
                st = PROTO_STYLES[proto]
                v, y = cdf(fcts(rows))
                p50 = percentile(fcts(rows), 50)
                mx  = np.max(fcts(rows))
                ax.plot(v, y * 100, color=color, linestyle=st["ls"], linewidth=1.8,
                        label=f"{st['label']}  p50={p50:.0f}  max={mx:.0f}")
        ax.set_title(label, fontsize=9)
        ax.set_xlabel("FCT (μs)")
        ax.set_ylabel("CDF (%)")
        ax.set_ylim(0, 100)
        ax.legend(fontsize=7)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    _save(fig, out_dir, "fig_mcc_ablation")


# ──────────────────────────────────────────────────────────────────────────────
# Summary table — print p50/p99/max for each protocol × workload
# ──────────────────────────────────────────────────────────────────────────────
def print_summary_table(results_root):
    PROTOS = ["swift", "fastflow", "fastflow+eqds", "fastflow+eqds+mcc",
              "fastflow+ra_qa", "fastflow+eqds+mcc+ra_qa"]
    INCAST_CASES = [(32, 128), (32, 1024), (100, 1024), (100, 4096)]

    print("\n{:35s}  {:>8s}  {:>8s}  {:>8s}  {:>5s}".format(
        "Workload / Protocol", "p50(us)", "p99(us)", "max(us)", "n"))
    print("-" * 75)

    # Permutation
    print("Perm 2MiB (1024 nodes, 8:1 OS):")
    for proto in PROTOS:
        if proto == "swift":
            rows = load_perm(results_root, proto, "perm_os8_2MiB_trim")
        else:
            rows = load_perm(results_root, proto, "perm_os8_2MiB")
        if not rows:
            continue
        v = fcts(rows)
        label = PROTO_STYLES.get(proto, {}).get("label", proto)
        print(f"  {label:33s}  {percentile(v,50):8.0f}  {percentile(v,99):8.0f}  {np.max(v):8.0f}  {len(v):5d}")

    # Incast
    for deg, siz in INCAST_CASES:
        print(f"Incast {deg}-deg {siz}KiB:")
        for proto in PROTOS:
            rows = load_incast(results_root, proto, deg, siz)
            if not rows:
                continue
            v = fcts(rows)
            label = PROTO_STYLES.get(proto, {}).get("label", proto)
            print(f"  {label:33s}  {percentile(v,50):8.0f}  {percentile(v,99):8.0f}  {np.max(v):8.0f}  {len(v):5d}")


# ──────────────────────────────────────────────────────────────────────────────
def _save(fig, out_dir, name):
    out_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("pdf", "png"):
        fig.savefig(out_dir / f"{name}.{ext}", dpi=150, bbox_inches="tight")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results",
                    help="Root results directory (default: results)")
    ap.add_argument("--out", default="plots",
                    help="Output directory for plots (default: plots)")
    args = ap.parse_args()

    results_root = Path(args.results)
    out_dir      = Path(args.out)

    print(f"Results root: {results_root}")
    print(f"Output:       {out_dir}")
    print()

    print("Generating fig1_perm_2MiB_cdf ...")
    plot_fig1_perm_cdf(results_root, out_dir)

    print("Generating fig5_incast_normalized ...")
    plot_fig5_incast(results_root, out_dir)

    print("Generating fig_raqa_ablation ...")
    plot_raqa_ablation(results_root, out_dir)

    print("Generating fig_mcc_ablation ...")
    plot_mcc_ablation(results_root, out_dir)

    print_summary_table(results_root)

    print(f"\nAll plots written to {out_dir}/")


if __name__ == "__main__":
    main()
