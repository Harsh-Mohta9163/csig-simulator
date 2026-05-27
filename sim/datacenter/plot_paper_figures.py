#!/usr/bin/env python3
"""
plot_paper_figures.py
Reproduce Bonato et al. 2024 (FASTFLOW paper) figures in the paper's exact style.

Protocols compared: Swift, FASTFLOW, FASTFLOW+EQDS, FASTFLOW+EQDS+Coflow

Figures:
  Fig 5  — Incast: line plot, y = normalized to theoretical best, x = log message size
  Fig 8  — Permutation: CDF plots (bisect permutation, different OS ratios / scenarios)
  Fig 9  — Alltoall: grouped bar chart with % above ideal labels
  Fig 10 — FASTFLOW+EQDS permutation: CDFs (2MiB homogeneous + 32MiB+64MiB)

Usage:
  python3 plot_paper_figures.py [--results-dir DIR] [--plots-dir DIR]
"""

import os
import argparse
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.lines import Line2D

# ─────────────────────────────────────────────────────────────────────────────
# Protocol config — 4 protocols matching paper's comparison set
# ─────────────────────────────────────────────────────────────────────────────

PROTOCOLS = [
    "swift",
    "eqds",
    "fastflow",
    "fastflow+eqds",
    "fastflow+eqds+mcc+coflow",
]

# Incast only compares FASTFLOW variants (pure EQDS uses different cwnd defaults
# that cause burst-induced trim and would need separate parameter tuning)
PROTOCOLS_INCAST = [
    "swift",
    "fastflow",
    "fastflow+eqds",
    "fastflow+eqds+mcc+coflow",
]

PROTO_DIRS = {
    "swift":                    "swift",
    "eqds":                     "eqds",
    "fastflow":                 "fastflow",
    "fastflow+eqds":            "fastflow_eqds",
    "fastflow+eqds+mcc+coflow": "fastflow_eqds_mcc_coflow",
}

PROTO_LABELS = {
    "swift":                    "Swift",
    "eqds":                     "EQDS",
    "fastflow":                 "FASTFLOW",
    "fastflow+eqds":            "FASTFLOW+EQDS",
    "fastflow+eqds+mcc+coflow": "FASTFLOW+EQDS+Coflow",
}

# Colors chosen to visually match the paper's palette
PROTO_COLORS = {
    "swift":                    "#e41a1c",   # red
    "eqds":                     "#984ea3",   # purple
    "fastflow":                 "#4daf4a",   # green
    "fastflow+eqds":            "#377eb8",   # blue
    "fastflow+eqds+mcc+coflow": "#ff7f00",   # orange
}

PROTO_MARKERS = {
    "swift":                    "o",
    "eqds":                     "v",
    "fastflow":                 "s",
    "fastflow+eqds":            "^",
    "fastflow+eqds+mcc+coflow": "D",
}

PROTO_LINES = {
    "swift":                    "-",
    "eqds":                     "-",
    "fastflow":                 "-",
    "fastflow+eqds":            "--",
    "fastflow+eqds+mcc+coflow": "-.",
}

plt.rcParams.update({
    "font.size": 10,
    "axes.titlesize": 10,
    "axes.labelsize": 10,
    "legend.fontsize": 8,
    "figure.dpi": 150,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "grid.linestyle": "--",
})

LINKSPEED_BPS = 800e9      # 800 Gbps
MTU_B = 4096

def ideal_single_fct_us(size_bytes):
    return size_bytes / (LINKSPEED_BPS / 8) * 1e6

# ─────────────────────────────────────────────────────────────────────────────
# FCT file parsing
# ─────────────────────────────────────────────────────────────────────────────

def load_fct(path):
    """Return sorted list of fct_us values."""
    data = []
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 10 and p[0] == "FCT":
                    data.append(float(p[7]))
    except FileNotFoundError:
        return []
    data.sort()
    return data


def load_timestamps(path):
    """Return (starts, finishes, fcts) as lists of µs values."""
    starts, finishes, fcts = [], [], []
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 10 and p[0] == "FCT":
                    starts.append(float(p[3]))
                    finishes.append(float(p[5]))
                    fcts.append(float(p[7]))
    except FileNotFoundError:
        return [], [], []
    return starts, finishes, fcts


def cdf_xy(data):
    n = len(data)
    return np.array(data), np.arange(1, n + 1) / n


def pstats(data):
    if not data:
        return None
    n = len(data)
    return {
        "n":    n,
        "p50":  data[n // 2],
        "p99":  data[int(n * 0.99)],
        "max":  data[-1],
        "mean": sum(data) / n,
    }

# ─────────────────────────────────────────────────────────────────────────────
# Fig 5 — Incast: normalized to theoretical best, log x-axis
# Paper style: 3 subplots (8:1, 32:1, 100:1), line+marker per protocol
# Y-axis: ideal_incast_FCT / mean_actual_FCT  (1.0 = perfect)
# X-axis: message size in KiB, log2 scale
# ─────────────────────────────────────────────────────────────────────────────

def plot_fig5_incast(results_dir, plots_dir):
    degrees = [8, 32, 100]
    # Sizes matching paper x-axis: 2^2 to 2^15 KiB
    sizes_kib = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768]

    os.makedirs(f"{plots_dir}/incast", exist_ok=True)

    fig, axes = plt.subplots(1, 3, figsize=(12, 3.5))
    fig.suptitle(
        "Message Scaling During Incast\n"
        "1024 Nodes · 800Gbps · 4KiB MTU",
        fontsize=10
    )

    degree_titles = {8: "8:1 Incast", 32: "32:1 Incast", 100: "100:1 Incast"}

    for ax_idx, deg in enumerate(degrees):
        ax = axes[ax_idx]

        # First pass: compute mean FCT — only include runs where ≥ 80% of flows
        # complete. Protocols with severe failure (< 80% completion) are excluded
        # at that (degree, size) point to avoid artificial spikes from lucky flows.
        mean_fcts = {}  # (proto, siz_kib) -> mean_fct_us
        MIN_COMPLETION = 0.80
        for proto in PROTOCOLS_INCAST:
            for siz_kib in sizes_kib:
                fct_file = os.path.join(
                    results_dir, "incast", PROTO_DIRS[proto],
                    f"incast_{deg}deg_{siz_kib}KiB.fct")
                data = load_fct(fct_file)
                if data and len(data) >= deg * MIN_COMPLETION:
                    mean_fcts[(proto, siz_kib)] = sum(data) / len(data)

        # Second pass: normalize to THEORETICAL BEST (perfect fair serialization)
        # theo_best = d × flow_size / link_bw  (all d flows sharing the link fairly)
        # normalized = 1.0 means the protocol achieved perfect throughput
        for proto in PROTOCOLS_INCAST:
            xs, ys = [], []
            for siz_kib in sizes_kib:
                if (proto, siz_kib) not in mean_fcts:
                    continue
                theo_best_us = deg * siz_kib * 1024 / (LINKSPEED_BPS / 8) * 1e6
                normalized = theo_best_us / mean_fcts[(proto, siz_kib)]
                xs.append(siz_kib)
                ys.append(normalized)

            if xs:
                ax.plot(xs, ys,
                        color=PROTO_COLORS[proto],
                        marker=PROTO_MARKERS[proto],
                        linestyle=PROTO_LINES[proto],
                        linewidth=1.6,
                        markersize=4,
                        label=PROTO_LABELS[proto])

        ax.set_xscale("log", base=2)
        ax.set_xlabel("Message Size (KiB)")
        if ax_idx == 0:
            ax.set_ylabel("Normalized to Theo. Best")
        ax.set_title(degree_titles[deg])
        ax.set_ylim(0.50, 1.02)
        ax.set_xlim(3, 40000)
        ax.xaxis.set_major_formatter(
            ticker.FuncFormatter(lambda x, _: f"$2^{{{int(np.log2(x))}}}$" if x >= 1 else ""))
        ax.grid(True, which="both", alpha=0.3, linestyle="--")

    # Shared legend on the first subplot
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        axes[0].legend(handles, labels, loc="lower right", fontsize=8)

    plt.tight_layout()
    out = f"{plots_dir}/incast/fig5_incast_normalized.pdf"
    plt.savefig(out, bbox_inches="tight")
    plt.savefig(out.replace(".pdf", ".png"), bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")


# ─────────────────────────────────────────────────────────────────────────────
# Fig 8 — Permutation: CDF plots, paper style
# 4 subplots: (a) 2:1 OS 32MiB, (b) 4:1 OS 32MiB,
#             (c) 8:1 OS 4x4MiB, (d) 8:1 OS 2MiB+one4MiB
# Labels on curves: "p50us, p99us"
# ─────────────────────────────────────────────────────────────────────────────

def _cdf_label(s, ms=False):
    """Format p50/p99 for curve label."""
    if ms:
        return f"{s['p50']/1000:.1f}ms, {s['p99']/1000:.1f}ms"
    return f"{s['p50']:.0f}us, {s['p99']:.0f}us"


def _plot_cdf_panel(ax, fct_files, title, xlabel_ms=False):
    """Plot CDF panel with paper-style labelled curves."""
    has_data = False
    for proto, fct_file in fct_files:
        data = load_fct(fct_file)
        if not data:
            continue
        has_data = True
        s = pstats(data)
        xs = np.array(data)
        if xlabel_ms:
            xs = xs / 1000
        ys = np.arange(1, len(data) + 1) / len(data)
        label = f"{PROTO_LABELS[proto]}\n{_cdf_label(s, ms=xlabel_ms)}"
        ax.plot(xs, ys,
                color=PROTO_COLORS[proto],
                linewidth=1.8,
                label=label)

    ax.set_ylabel("CDF (%)")
    ax.set_xlabel(f"Flow Completion Time ({'ms' if xlabel_ms else 'µs'})")
    ax.set_title(title)
    ax.set_ylim(0, 1.05)
    return has_data


def plot_fig8_permutation(results_dir, plots_dir):
    """Fig 8: Bisect permutation CDF — different OS ratios and scenarios."""
    os.makedirs(f"{plots_dir}/permutation", exist_ok=True)

    scenarios = [
        {
            "label": "(a)",
            "title": "Bisect Permutation 2:1 OS · 32MiB\n1024 Nodes · 800Gbps · 4KiB MTU",
            "key": "perm_os2_32MiB",
            "ms": False,
        },
        {
            "label": "(b)",
            "title": "Bisect Permutation 4:1 OS · 32MiB\n1024 Nodes · 800Gbps · 4KiB MTU",
            "key": "perm_os4_32MiB",
            "ms": False,
        },
        {
            "label": "(c)",
            "title": "Bisect Perm. 8:1 OS · 4×4MiB\n1024 Nodes · 800Gbps · 4KiB MTU",
            "key": "perm_os8_4x4MiB",
            "ms": False,
        },
        {
            "label": "(d)",
            "title": "Bisect Perm. 8:1 OS · 2MiB + one 4MiB\n1024 Nodes · 800Gbps · 4KiB MTU",
            "key": "perm_os8_2MiB_one4MiB",
            "ms": False,
        },
    ]

    fig, axes = plt.subplots(1, 4, figsize=(16, 3.8))
    fig.suptitle("Flow completion time for different permutations on fat trees with different oversubscription ratios.",
                 fontsize=9)

    any_plotted = False
    for ax_idx, sc in enumerate(scenarios):
        ax = axes[ax_idx]
        fct_files = []
        for proto in PROTOCOLS:
            fct_file = os.path.join(
                results_dir, "permutation", PROTO_DIRS[proto],
                f"{sc['key']}.fct")
            fct_files.append((proto, fct_file))

        has = _plot_cdf_panel(ax, fct_files,
                              f"{sc['label']} {sc['title'].split(chr(10))[0]}",
                              xlabel_ms=sc["ms"])
        if has:
            any_plotted = True
            ax.legend(fontsize=7, loc="upper left",
                      framealpha=0.85, handlelength=1.5)
        else:
            ax.text(0.5, 0.5, "Data not yet\navailable",
                    ha="center", va="center", transform=ax.transAxes,
                    fontsize=9, color="gray")
            ax.set_title(f"{sc['label']} {sc['title'].split(chr(10))[0]}")

    plt.tight_layout()
    out = f"{plots_dir}/permutation/fig8_permutation_cdf.pdf"
    plt.savefig(out, bbox_inches="tight")
    plt.savefig(out.replace(".pdf", ".png"), bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")



# ─────────────────────────────────────────────────────────────────────────────
# Fig 9 — Alltoall: grouped bar chart with % above ideal labels
# Matches paper: bars grouped by k, protocols as different colors,
# grey dashed line = ideal, % labels on each bar
# ─────────────────────────────────────────────────────────────────────────────

def plot_fig9_alltoall(results_dir, plots_dir):
    os.makedirs(f"{plots_dir}/alltoall", exist_ok=True)

    ks = [1, 2, 8, 16]
    # alltoall only compares FASTFLOW variants + Swift (no EQDS alltoall data)
    PROTOCOLS_A2A = ["swift", "fastflow", "fastflow+eqds", "fastflow+eqds+mcc+coflow"]

    # Theoretical ideal JCT:
    # 127 flows × 1MiB per sender; 8:1 OS → effective uplink = 800Gbps/8 = 100 Gbps
    # Byte rate at full link = LINKSPEED_BPS/8; with 8:1 OS divide by 8 again.
    # ideal = 127 × 1MiB bytes / (100 Gbps / 8 bytes_per_bit / 8 OS)
    # = 127 × 1MiB × 8 bits/byte × 8 OS / LINKSPEED_BPS × 1e3 ms/s
    ideal_ms_os8 = 127 * 1048576 * 8 * 8 / LINKSPEED_BPS * 1e3  # ≈ 10.65 ms

    # Compute JCT for each protocol × k
    jct = {}  # (proto, k) -> ms
    for proto in PROTOCOLS_A2A:
        for k in ks:
            path = os.path.join(results_dir, "alltoall", PROTO_DIRS[proto],
                                f"a2a_128n_k{k}.fct")
            starts, finishes, fcts = load_timestamps(path)
            if starts and len(starts) >= 16256 * 0.90:
                # Accept ≥ 90% completion; JCT = last finish - first start
                jct[(proto, k)] = (max(finishes) - min(starts)) / 1000  # ms
            else:
                jct[(proto, k)] = None

    fig, ax = plt.subplots(figsize=(9, 4.5))

    n_proto = len(PROTOCOLS_A2A)
    x = np.arange(len(ks))
    bar_w = 0.18
    offsets = np.linspace(-(n_proto - 1) / 2 * bar_w,
                          (n_proto - 1) / 2 * bar_w, n_proto)

    for p_idx, proto in enumerate(PROTOCOLS_A2A):
        for k_idx, k in enumerate(ks):
            val = jct.get((proto, k))
            if val is None:
                continue
            ax.bar(x[k_idx] + offsets[p_idx], val, bar_w,
                   color=PROTO_COLORS[proto],
                   label=PROTO_LABELS[proto] if k_idx == 0 else "",
                   zorder=3)
            # % above ideal label
            pct = (val - ideal_ms_os8) / ideal_ms_os8 * 100
            ax.text(x[k_idx] + offsets[p_idx], val + 0.15,
                    f"{pct:.0f}%", ha="center", va="bottom",
                    fontsize=6.5, fontweight="bold")

    # Ideal line
    ax.axhline(ideal_ms_os8, color="gray", linestyle="--",
               linewidth=1.5, zorder=2, label=f"Ideal ({ideal_ms_os8:.1f} ms)")

    ax.set_xlabel("All-to-all parallel window size")
    ax.set_ylabel("Job Completion Time (ms)")
    ax.set_title(
        "Performance in several all-to-all scenarios · 1MiB message\n"
        "128 Nodes · 8:1 OS · 800Gbps · 4KiB MTU"
    )
    ax.set_xticks(x)
    ax.set_xticklabels([f"k={k}" for k in ks])
    ax.set_ylim(0, max(v for v in jct.values() if v) * 1.18)

    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles, labels, loc="upper right", fontsize=8,
              framealpha=0.9, title="Algorithm")

    plt.tight_layout()
    out = f"{plots_dir}/alltoall/fig9_alltoall_bar.pdf"
    plt.savefig(out, bbox_inches="tight")
    plt.savefig(out.replace(".pdf", ".png"), bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")


# ─────────────────────────────────────────────────────────────────────────────
# Fig 10 — FASTFLOW+EQDS permutation CDFs
# (a) All flows 2MiB  (b) One longer flow at 64MiB (rest 32MiB)
# ─────────────────────────────────────────────────────────────────────────────

def plot_fig10_eqds_fastflow(results_dir, plots_dir):
    os.makedirs(f"{plots_dir}/permutation", exist_ok=True)

    panels = [
        {
            "title": "Bisect Permutation 8:1 OS · 2MiB\n1024 Nodes · 800Gbps · 4KiB MTU",
            "key": "perm_os8_2MiB_bisect",
            "ms": False,
            "sub": "(a) All flows 2MiB.",
        },
        {
            "title": "Permutation 8:1 OS · 32MiB\n1024 Nodes · 800Gbps · 4KiB MTU",
            "key": "perm_os8_32MiB",
            "ms": True,
            "sub": "(b) All flows 32MiB.",
        },
    ]

    fig, axes = plt.subplots(1, 2, figsize=(8, 3.8))
    fig.suptitle("Augmenting EQDS with sender-based mechanisms.", fontsize=9)

    for ax_idx, panel in enumerate(panels):
        ax = axes[ax_idx]
        fct_files = []
        for proto in PROTOCOLS:
            fct_file = os.path.join(
                results_dir, "permutation", PROTO_DIRS[proto],
                f"{panel['key']}.fct")
            fct_files.append((proto, fct_file))

        has = _plot_cdf_panel(ax, fct_files, panel["sub"], xlabel_ms=panel["ms"])
        if has:
            ax.legend(fontsize=7, loc="upper left", framealpha=0.85)
        else:
            ax.text(0.5, 0.5, "Data not yet\navailable",
                    ha="center", va="center", transform=ax.transAxes,
                    fontsize=9, color="gray")
            ax.set_title(panel["sub"])

    plt.tight_layout()
    out = f"{plots_dir}/permutation/fig10_eqds_fastflow.pdf"
    plt.savefig(out, bbox_inches="tight")
    plt.savefig(out.replace(".pdf", ".png"), bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-dir", default="results/paper")
    parser.add_argument("--plots-dir", default="plots/paper")
    args = parser.parse_args()

    os.makedirs(args.plots_dir, exist_ok=True)

    print("=== Generating paper figures ===\n")

    print("Fig 5: Incast (normalized to theoretical best, log x-axis)")
    plot_fig5_incast(args.results_dir, args.plots_dir)

    print("\nFig 8: Permutation CDF (bisect scenarios)")
    plot_fig8_permutation(args.results_dir, args.plots_dir)

    print("\nFig 9: Alltoall bar chart")
    plot_fig9_alltoall(args.results_dir, args.plots_dir)

    print("\nFig 10: FASTFLOW+EQDS permutation")
    plot_fig10_eqds_fastflow(args.results_dir, args.plots_dir)

    print(f"\n=== Done. Plots in: {args.plots_dir}/ ===")


if __name__ == "__main__":
    main()
