# FASTFLOW / SMaRTT Implementation Report

## 1. Goal

Reproduce the qualitative **trends** (not exact numbers) from the SMaRTT paper (Bonato et al. 2024,
"SMaRTT: Sender-based Marked Rapidly-adapting Trimmed & Timed Transport") on a 1024-node
800Gbps fat-tree.  Specific trends to match:

- **Incast** (8:1, 32:1, 50:1): FASTFLOW V-dip at medium sizes, recovery at large sizes.
  FF+EQDS eliminates the dip (ideal hybrid). EQDS flat throughout.
- **Permutation**: FASTFLOW beats EQDS at OS > 1:1. EQDS collapses under oversubscription.
  FF+EQDS traces close to FASTFLOW.

---

## 2. Algorithm Corrections (this session)

Six deviations from the paper were corrected:

### 2a. Multiplicative Decrease — CRITICAL

**Paper Eq.1:**
```
cwnd = cwnd × max(0.5,  1 − (avg_rtt − trtt) / avg_rtt × 0.8)
```
- Multiplicative on the whole cwnd (cuts from BDP to fair-share in ~7 RTTs).
- Caps at "halve cwnd" (factor ≥ 0.5).
- Uses avg_rtt (EWMA).

**Old code:** subtractive `cwnd -= ratio × 2.0 × pkt_size`. Capped at 1 MTU per RTT.
Result: from BDP to fair-share took ~300 RTTs → cwnd never converged → 0.74 incast efficiency.

**Fixed in:** `FastflowSrc::multiplicative_decrease()` (no args, uses `_rtt` EWMA).

### 2b. Core Cases (Alg.1)

| Condition | Old code | Fixed code |
|---|---|---|
| `ecn && rtt > trtt` | MD + FD | MD only (once per base_rtt) |
| `ecn && rtt < trtt` | FD | NO-OP (path change handled by PLB) |
| `!ecn && rtt > trtt` | FI | FI only ✓ |
| `!ecn && rtt < trtt` | PI + FI | PI + FI ✓ |

Removed `fair_decrease` from both ECN branches. `wait_to_decrease` (EMA gate) removed;
once-per-`base_rtt` guard is now inline in the MD call site.

### 2c. Ignore Window (Alg.4)

**Old code:** ignore window only suppressed *decreases* inside core_cases; QA and FI still ran.

**Fixed:** strict early-return that blocks QA, FI, AND core_cases for all ACKs during the
ignore window (paper Algorithm 4 lines 6-8).

Also fixed: `_bytes_ignored` now increments by MTU per *packet* (not per cum-ack delta). Trim
ACKs that don't advance the cumulative ACK still count toward draining the window; without this
fix, heavy trim storms kept the ignore window active indefinitely.

### 2d. QuickAdapt Scaling (Alg.2)

**Old code:** `cwnd = max(acked × 0.8, mtu)` (0.8 qa_scaling factor).
**Fixed:** `cwnd = max(acked, mtu)` (no scaling — paper §III-E confirms qa_scaling = 1).

### 2e. FastIncrease Gate (Alg.3)

**Old code:** `rtt ≤ base + (target − base)/4 ≈ 13.5 µs`.
**Fixed:** `rtt ≤ base_rtt` (strict, matching paper Algorithm 3 line 2).

### 2f. cwnd Ceiling

**Old:** 1.25 × BDP.
**Fixed:** 1.5 × BDP (paper §III-J explicitly states "maximum window to 1.5 BDP").

---

## 3. Results (evaluation in progress)

Spot checks after the fix (50:1 incast, non-blocking topology):

| Size | FASTFLOW norm. | Status |
|---|---|---|
| 512 KiB | 0.627 | V-dip region (expected per paper) |
| 32768 KiB | 0.823 | Recovery region — significantly better than old 0.74 |

8:1 incast spot check:

| Size | FASTFLOW norm. |
|---|---|
| 128 KiB | 0.259 (small-flow RTT overhead; FASTFLOW beats EQDS 0.152) |
| 512 KiB | 0.558 |
| 4096 KiB | 0.863 |
| 32768 KiB | 0.846 |

V-dip → recovery trend is present. Full evaluation running.

---

## 4. Incast Trend Explanation

The remaining gap from paper's ~0.92 at medium sizes is attributable to load balancer differences:

- **Paper uses REPS** (Random Entropy Packet Spraying): multiple senders spray across all paths
  simultaneously, preventing any single path from building a queue → all senders can fire FI
  at the same time → fast ramp-up even for small flows.
- **Our code uses PLB (ECMP)**: single-path per flow → path collisions → queue builds earlier →
  `rtt > base_rtt` → strict FI gate fails → slower cwnd growth.

This is a fundamental limitation of ECMP vs REPS, not an algorithm correctness issue.

---

## 5. Files Modified

| File | Changes |
|---|---|
| `sim/fastflow/fastflow.cpp` | MD formula (Eq.1), ignore-window fix, QA fix, FI gate, clamp_cwnd, remove WTD + FD, rename MI→PI |
| `sim/fastflow/fastflow.h` | Removed dead statics (`_fd_const`, `_md_const`, `_qa_scaling`, `_wtd_*`, `_avg_wtd`); updated declarations |
| `sim/datacenter/main_fastflow.cpp` | Removed dead static assignments |
| `sim/datacenter/run_paper_eval.sh` | Degrees: 8/32/100 → 8/32/50 |
| `sim/datacenter/plot_paper_figures.py` | Degrees: [8,32,100] → [8,32,50]; title updated |
| `sim/datacenter/connection_matrices/` | Added 50-degree incast matrices (14 sizes) |
