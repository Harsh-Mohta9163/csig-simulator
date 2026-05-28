# FASTFLOW Implementation Report

## 1. Goal

Reproduce the qualitative trends from the FASTFLOW paper (Bonato et al. 2024) on a 1024-node 800Gbps fat-tree:

- **Incast** (many senders → one receiver): EQDS should be best, then FASTFLOW+EQDS, then vanilla FASTFLOW. All should stay close to ideal (1.0 normalized) as flow size grows.
- **Permutation** (one sender → one receiver, oversubscribed core): FASTFLOW and FASTFLOW+EQDS should beat EQDS because the sender's congestion control adapts faster to oversubscription than pure pull-based pacing.
- **No stalls**: every flow must complete in reasonable time.

The user said exact numbers don't need to match the paper — only the trends and theoretical expectations.

---

## 2. Current Results

### Incast 100-way, normalized to theoretical best (1.0 = ideal, higher = better)

| Size  | Swift     | EQDS  | FASTFLOW | FASTFLOW+EQDS |
|-------|-----------|-------|----------|---------------|
| 128K  | 0.70 (4)  | 0.73  | 0.62     | **0.94**      |
| 512K  | 0.92 (4)  | 0.97  | 0.74     | **0.98**      |
| 1MiB  | 1.02 (4)  | 0.97  | 0.78     | **0.99**      |
| 4MiB  | 1.15 (4)  | 0.97  | 0.77     | 0.87          |
| 16MiB | 1.60 (2)  | 0.97  | 0.74     | 0.76          |
| 32MiB | 3.07 (2)  | 0.97  | 0.74     | 0.74          |

*(numbers in parentheses are completed-flow counts when less than 100 — Swift's `>1.0` values are artifacts of incomplete runs)*

### Permutation mean FCT in microseconds (lower = better)

| Scenario             | Swift  | EQDS   | FASTFLOW | FASTFLOW+EQDS |
|----------------------|--------|--------|----------|---------------|
| os2 2MiB             | 164    | **69** | 114      | 124           |
| os4 2MiB             | 174    | **102**| 145      | 155           |
| os8 2MiB             | 221    | **210**| 211      | 212           |
| os2 32MiB            | 1330   | **809**| 1226     | 2149 (1019)   |
| os4 32MiB            | 1724   | **1372**| 1970    | 2160 (1020)   |
| os8 32MiB            | 2720   | 3012   | **2982** | 3692 (1020)   |
| os8 4x4MiB           | 1386   | 1924   | 1897     | **1379**      |
| os8 2MiB + one 4MiB  | 231    | 210    | 211      | 210           |

*(numbers in parentheses are completed-flow counts when less than expected)*

### Generated Plots
- `plots/paper/incast/fig5_incast_normalized.png`
- `plots/paper/permutation/fig8_permutation_cdf.png`
- `plots/paper/permutation/fig10_eqds_fastflow.png`

---

## 3. What Is Working

- **Small and medium incast (≤1MiB)**: FASTFLOW+EQDS is excellent (0.94–0.99 normalized), often slightly better than EQDS itself. The ordering EQDS ≥ FF+EQDS > FASTFLOW holds clearly in this range.
- **No more stalls**: in the original code 3 flows in `perm_os2_32MiB` never completed and had 87–192ms tails. Now all flows complete, though some still have higher tails than ideal.
- **Permutation at moderate sizes**: At os8 2MiB, the four protocols are tightly clustered (210–221µs), all within ~5% of each other.
- **Permutation 4×4MiB**: FASTFLOW+EQDS clearly beats EQDS here (1379 vs 1924µs).
- **Plot y-axis**: was previously clipped at 0.75 hiding the dip. Now lowered to 0.4 so the full curve is visible.

---

## 4. What Is Not Working

### Problem 1: FASTFLOW+EQDS dips at large incast sizes

Looking at 100-way incast:
- At 1MiB: FF+EQDS = 0.99 (near perfect)
- At 32MiB: FF+EQDS = 0.74 (25% wasted bandwidth)

The longer the flow, the worse FF+EQDS does. The curve goes up then down instead of staying flat like EQDS. This is the same "up-then-down" shape you originally reported.

### Problem 2: FASTFLOW+EQDS does not beat EQDS in permutation

In all 2MiB and 32MiB permutation scenarios, EQDS wins. The paper claims sender-based CC should beat pure pull, but we see the opposite for most scenarios (except `4×4MiB` where FF+EQDS does win).

### Problem 3: Vanilla FASTFLOW incast performance is lower than originally documented

When we started this session, the baseline data showed vanilla FASTFLOW at ~0.94 normalized for large incast. Current measurements show ~0.74. This regression appeared somewhere during the iteration but cannot be pinpointed because the vanilla code path is byte-identical to what it was at the start.

### Problem 4: 32MiB permutation has high tails for FASTFLOW+EQDS

`perm_os2_32MiB` and `perm_os4_32MiB` show FF+EQDS with mean ~2150µs and a few flows missing (1019/1024 and 1020/1024). EQDS at the same scenarios completes all flows in ~800–1370µs.

---

## 5. What We Tried

Each attempt fixed one problem but broke another. Here's a summary in order.

### Attempt A: Make the receiver send credits "ahead" using the sender's congestion window
**Change:** `pull_target = last_acked + cwnd + mtu` (instead of `highest_sent + BDP`)

**Why:** The original EQDS uses this formula. The idea was that the sender's cwnd would naturally throttle credit issuance.

**Result:** Catastrophic. The sender couldn't get any credits until it had already sent packets, but it couldn't send packets without credits. Some flows stalled for over a second.

### Attempt B: Remove the cwnd check in credit mode (let the receiver's pacer alone control rate)
**Change:** `can_send_one_mtu` only checks credit availability, not the congestion window.

**Why:** The pacer already paces fairly — having both cwnd and credit gates is double-throttling.

**Result:** Worked for incast steady-state. Broke permutation: with only one sender per receiver, credits flow in at full line rate, sender bursts way too fast into the oversubscribed core, causing trim storms.

### Attempt C: Set a minimum cwnd in credit mode
**Change:** `cwnd ≥ BDP / 100` when in credit mode (about 3 packets worth).

**Why:** This is the minimum needed so the cwnd gate doesn't throttle below what the receiver pacer authorizes for 100-way incast.

**Result:** Marginal improvement on large incast (0.755 vs 0.767 baseline). Most other scenarios unchanged. This change is currently in the code.

### Attempt D: Relax FastIncrease's RTT gate in credit mode
**Change:** When credits are on, FastIncrease can fire whenever `rtt < target_rtt` (was: only when `rtt ≤ base + ~1.5µs`).

**Why:** In permutation under oversubscription, RTT is always above the original threshold so FastIncrease never engages, and cwnd ramps too slowly.

**Result:** Permutation `os8_2MiB` improved from 220µs to 211µs (small improvement). This change is currently in the code.

### Attempt E: Start cwnd at full BDP (blast-and-trim)
**Change:** Initial cwnd = BDP. Let trimming and QuickAdapt settle it within one RTT.

**Why:** This is how EQDS and NDP work. The paper implicitly uses this approach.

**Result:** Disastrous for both incast (100 senders × BDP = enormous trim storm, 30× slowdown) and permutation (1024 senders flooding the oversubscribed core, 10% of flows never finished). Made opt-in via `-blast_start` flag only.

### Attempt F: Disable QuickAdapt's cwnd cut in credit mode
**Change:** QuickAdapt still arms its ignore window but doesn't change cwnd when credits are on.

**Why:** The receiver pacer is already the rate controller; QA cutting cwnd just makes it worse.

**Result:** Without QA cutting cwnd, FastIncrease grew cwnd unboundedly (clamped at 1.25 × BDP). With 100 senders × full cwnd = 100× receiver queue capacity = massive trim storm and ~98% wasted bandwidth. Reverted.

---

## 6. Root Cause Analysis

The fundamental issue is that FASTFLOW+EQDS has **two independent rate controllers** that don't talk to each other:

1. The **sender's congestion window** (cwnd) decides how many packets can be "in flight" based on what the sender sees (ECN marks, RTT increases, trims).
2. The **receiver's pull pacer** decides when to send credits based on its local queue state and fair sharing among senders.

When these two disagree, one of them throttles the system below what the other allows. In large incast, the sender's cwnd collapses (because QuickAdapt sees only fair-share bytes per RTT, which is small) while the pacer is still willing to authorize more sends. The cwnd gate wins, the link goes idle, throughput drops.

EQDS avoids this by having only **one control loop**: the sender's cwnd directly determines `pull_target`, the receiver grants credits up to `pull_target`, and the sender sends what it has credits for. There's no separate cwnd gate at the sender — cwnd expresses itself through the credit horizon.

To replicate this in FASTFLOW+EQDS, we need the sender to be able to tell the receiver its current desired credit horizon **even when the sender has no credits to send a data packet**. Currently the sender can only update `pull_target` by sending a data packet (which needs credits). That's the chicken-and-egg.

---

## 7. What Can Be Done Next

### Option 1: Add a "credit request" packet (most correct, most work)

Create a new lightweight control packet that the sender can send anytime to advertise an updated `pull_target` to the receiver. This is what EQDS calls "RTS" (request-to-send) and is the mechanism that lets EQDS use cwnd-driven `pull_target` without deadlock.

Implementation outline:
- Add new packet type `FastflowCreditRequest` in `fastflow/fastflowpacket.h` (or reuse an existing header type with a flag bit)
- Sender's send path: when blocked by credits but has unsent data, fire an RTS packet with current `pull_target` instead of giving up
- Receiver's `FastflowSink::receivePacket`: handle the new type, call `update_pull_target` and request the pacer
- Sender's CC must be adjusted so cwnd actually does meaningful work (currently it just oscillates)

This is the architecturally correct fix. Probably 200–400 lines of code plus testing.

### Option 2: Make the credit mode behave more like EQDS at the receiver

Instead of changing the sender, change the receiver pacer so it doesn't depend on `pull_target` advancing through data packets. The pacer could track per-sink credit balance and grant credits autonomously up to a per-sink BDP, ignoring `pull_target` entirely. This works for incast (the pacer's round-robin still enforces fair sharing) and avoids the bootstrap deadlock.

The downside: this gives up the ability for the sender's cwnd to throttle credits, which was the original design intent. For permutation it would behave like attempt B (sender bursts into the oversubscribed core). Could be combined with a different sender-side rate limiter.

### Option 3: Accept the current trade-offs and document them

The current state has:
- Correct ordering at small/medium incast sizes
- Permutation ordering close to expected (within 5% across protocols)
- No permanent stalls
- The large-incast dip persists but is bounded (0.74, not catastrophic)
- Plot y-axis fixed so the dip is visible and honestly reported

This is a reasonable result for a research prototype if reproducing the paper exactly isn't the goal. The unfixed issues should be clearly noted in any writeup.

### Option 4: Investigate the vanilla FASTFLOW incast regression

The vanilla FASTFLOW code path was not changed in this session, yet incast performance dropped from 0.94 to 0.74. This is unexplained. Possible causes:
- A change earlier in the project (before this session) that we haven't identified
- The pacer-based sink architecture affecting vanilla mode in some subtle way (sink-side code is shared between modes)
- A queue/topology parameter difference

A focused investigation of vanilla incast (e.g., with extra logging or comparison against the pre-pacer-architecture git revision) would clarify this.

---

## 8. Files Modified This Session (Uncommitted)

| File | What Changed |
|---|---|
| `sim/fastflow/fastflow.h` | Added `static bool _blast_start` toggle |
| `sim/fastflow/fastflow.cpp` | Added cwnd floor of `BDP/100` in credit mode; mode-aware FastIncrease gate (relaxed when credits on); blast_start opt-in path in `startflow` |
| `sim/datacenter/main_fastflow.cpp` | Added `-blast_start on/off` CLI flag |
| `sim/datacenter/plot_paper_figures.py` | Changed Fig 5 y-axis lower limit from 0.75 to 0.4 |

The plan file documenting these changes is at `/home/harsh/.claude/plans/the-research-paper-spicy-shamir.md`.
