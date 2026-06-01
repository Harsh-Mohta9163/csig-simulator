// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// FASTFLOW implementation. See fastflow.h for module-level docs.

#include "fastflow.h"
#include "ecn.h"

#include <algorithm>
#include <cmath>
#include <iostream>

// ===========================================================================
//                        Static parameter defaults
// ===========================================================================
//
// These match the FASTFLOW paper Sec 3.5 reference settings. main_fastflow
// recomputes _bdp_bytes, _base_rtt, _target_rtt, _fi_const, _md_const from
// the actual topology before flows start, so these defaults only matter for
// unit tests that don't initialise the topology first.

uint16_t FastflowSrc::_mtu          = 4000;
double   FastflowSrc::_fi_const     = 0.25;
uint32_t FastflowSrc::_k_fastinc    = 2;
simtime_picosec FastflowSrc::_base_rtt   = timeFromUs((uint32_t)12);
simtime_picosec FastflowSrc::_target_rtt = timeFromUs((uint32_t)18);
uint32_t FastflowSrc::_bdp_bytes    = 1500000;
bool     FastflowSrc::_enable_ra_qa   = false;
bool     FastflowSrc::_enable_credits = false;
bool     FastflowSrc::_enable_mcc     = false;
bool     FastflowSrc::_trim_supported = true;
bool     FastflowSrc::_blast_start    = false;

// ===========================================================================
//                        FastflowSrc — constructor
// ===========================================================================
FastflowSrc::FastflowSrc(FastflowRtxTimerScanner& rtx_scanner,
                         TrafficLogger* pktlogger, EventList& eventlist)
    : EventSource(eventlist, "fastflow_src"),
      _flow(pktlogger),
      _pacer(*this, eventlist),
      _traffic_logger(pktlogger)
{
    _established   = false;
    _route         = NULL;
    _sink          = NULL;
    _path_index    = 0;
    _plb           = false;
    _plb_last_good = 0;
    _plb_interval  = timeFromSec(1);

    _highest_sent  = 0;
    _last_acked    = 0;
    _flow_size     = ((uint64_t)1) << 62;
    _packets_sent  = 0;

    // Start at 1 MTU; MI+FI ramps to BDP in ~8 RTTs. Starting at BDP causes
    // a burst storm when many flows fire simultaneously (common in perm/a2a).
    _cwnd          = _mtu;
    _acked         = 0;
    _bytes_ignored = 0;
    _bytes_to_ignore = 0;
    _trigger_qa    = false;
    _fast_inc_count = 0;
    _in_fast_inc   = false;
    _qa_end        = 0;
    _last_decrease = 0;

    _rtt           = 0;
    _rto           = timeFromUs((uint32_t)200);
    _mdev          = 0;
    // Lower bound on RTO. We use 25us because base RTT at 800Gbps fat-tree
    // is ~12us — a hole stuck because its trim ACK was dropped should be
    // retried after ~2 RTTs, not 100us+.
    _min_rto       = timeFromUs((uint32_t)25);
    _RFC2988_RTO_timeout = 0;
    _rtx_timeout_pending = false;
    _retransmit_cnt = 0;
    _drops         = 0;
    _retransmits_dbg = 0;
    _blocked_rtx_dbg = 0;

    // Initial burst = 1 MTU when credits are enabled. This bootstraps the
    // credit loop: the first packet arrives at the receiver, which issues a
    // BDP-sized credit grant back to the sender. The grant fills the pipeline
    // Initial speculative burst — matches EQDS's _credit_spec = _maxwnd = 50×MTU.
    // Small flows complete entirely from speculative credit (no credit handshake).
    // Large flows: speculative is consumed in 1 RTT, then per-packet credit kicks in.
    // For 50:1 incast: 50×50MTU = 2500 pkts vs 500-pkt queue → trim storm that
    // QuickAdapt + RTS recovery resolves within a few RTTs.
    // When credits are disabled, allow unlimited sending (cwnd is the only gate).
    _initial_burst_remaining = _enable_credits ? (uint64_t)(8 * _mtu) : ((uint64_t)1 << 62);
    _receiver_credits = 0;
    _last_rts_time = 0;
    _last_credit_time = 0;
    _credit_ema = 0;
    _msg_start_time = 0;
    _msg_acked      = 0;
    _msg_target_bw  = 0.0;
    _msg_tolerance  = 0.85;

    _start_time     = 0;
    _flow_finished  = false;
    _end_trigger    = NULL;

    _pacing_delay   = 0;
    _rtx_scanner    = &rtx_scanner;
    // Registration deferred to startflow() so the scanner only iterates
    // over active flows. Trigger-chained alltoall scenarios create thousands
    // of flows upfront; scanning all of them every 50us is prohibitively slow.

    _flow.set_id(get_id());
    _nodename = "fastflow_src" + std::to_string(get_id());
}

// ===========================================================================
//                        Connection setup
// ===========================================================================
void
FastflowSrc::connect(const Route& routeout, const Route& routeback,
                     FastflowSink& sink, simtime_picosec starttime) {
    _sink  = &sink;
    Route* new_route = routeout.clone();
    new_route->push_back(&sink);
    _route = new_route;
    sink.connect(*this, routeback);

    _start_time = starttime;
    if (starttime != TRIGGER_START) {
        eventlist().sourceIsPending(*this, starttime);
    }
}

void
FastflowSrc::set_paths(vector<const Route*>* rt_list) {
    _paths.clear();
    for (size_t i = 0; i < rt_list->size(); i++) {
        _paths.push_back(rt_list->at(i));
    }
}

void
FastflowSrc::set_flowsize(uint64_t bytes) {
    _flow_size = bytes;
}

// ===========================================================================
//                        Trigger interface
// ===========================================================================
void
FastflowSrc::activate() {
    _start_time = eventlist().now();
    startflow();
}

void
FastflowSrc::set_end_trigger(Trigger& trigger) {
    _end_trigger = &trigger;
}

void
FastflowSrc::doNextEvent() {
    if (_rtx_timeout_pending) {
        // Treat the timeout exactly like a packet trim: arm QuickAdapt,
        // bump retransmit_cnt, retransmit the next-expected byte.
        _rtx_timeout_pending = false;
        _trigger_qa = true;

        if (_cwnd > _mtu) _cwnd -= _mtu;
        clamp_cwnd();

        _retransmit_cnt++;
        FastflowPacket::seq_t sq = _last_acked + 1;
        if (can_retransmit(sq)) {
            retransmit_seqno(sq);
        }
        send_packets();   // try to push more after recovery
        return;
    }
    startflow();
}

void
FastflowSrc::startflow() {
    if (_established) return;
    _established = true;
    _rtx_scanner->registerSrc(*this);
    if (_start_time == 0 || _start_time == TRIGGER_START) {
        _start_time = eventlist().now();
    }
    _msg_start_time = eventlist().now();
    _msg_acked = 0;
    // Blast-start is opt-in via -blast_start flag only. Unconditionally opening
    // cwnd to BDP catastrophically degrades vanilla FASTFLOW: 100 senders × BDP
    // in incast = trim storm (30× slowdown); 1024 senders × BDP in permutation
    // overwhelms the 8:1 oversubscribed core, ~10% of flows fail to complete.
    // 1 MTU start with strict FastIncrease gating ramps to BDP in ~10 RTTs
    // without burst storms.
    if (_blast_start) {
        _cwnd = _bdp_bytes;
    }
    send_packets();
}

// ===========================================================================
//                        Main control loop (Algorithm 1)
// ===========================================================================
//
// Order matches the paper exactly:
//   1) bookkeeping (acked, bytes_ignored)
//   2) if trim/timeout -> handle_trim
//   3) else (normal ACK):
//        bypass while in post-QA ignore window
//        compute can_decrease (WTD)
//        try QuickAdapt, then FastIncrease (either may short-circuit)
//        4-case logic on (ECN, RTT vs trtt)
//   4) clamp cwnd into [MTU, 1.25 BDP]

void
FastflowSrc::handle_ack(FastflowAck& ack) {
    simtime_picosec now = eventlist().now();

    // Bytes the ACK represents
    uint64_t new_bytes = (ack.ackno() > _last_acked)
                             ? (ack.ackno() - _last_acked)
                             : 0;
    _acked         += new_bytes;
    // Paper Alg.4 line 3: bytes_ignored += p.size for EVERY packet (ACK or trim).
    // Using MTU so trim ACKs (which don't advance cum-ack, giving new_bytes=0) also
    // drain the ignore window. Without this, heavy trim storms stall the ignore window
    // indefinitely because new_bytes=0 for each trim and bytes_ignored never grows.
    _bytes_ignored += (uint64_t)_mtu;
    _msg_acked     += new_bytes;
    if (ack.ackno() > _last_acked) {
        _last_acked = ack.ackno();
        _RFC2988_RTO_timeout = now + _rto;
        if (_last_acked >= _highest_sent) {
            _RFC2988_RTO_timeout = 0;
        }
        prune_blockuntil();
    }
    drain_pending_rtx();

    // Trim / timeout path (paper Algo 1 lines 28-34). Note: we do NOT call
    // send_packets() here. The retransmit issued by handle_trim is the only
    // data this ACK should put on the wire — calling send_packets would
    // double-emit (retransmit + new data per trim) and produce a multiplicative
    // storm because trims travel faster than clean ACKs.
    if (ack.is_trimmed()) {
        handle_trim(ack);
        clamp_cwnd();
        maybe_report_completion();
        return;
    }

    // RTT sample (timestamp echo)
    simtime_picosec rtt_sample = now - ack.ts_echo();
    update_rtt(rtt_sample);

    // PLB / LOAD_BALANCER_PATH_CHANGE (paper Alg.1 ecn&&rtt<trtt case):
    // switch ECMP path when ECN marks arrive but delay is still below target.
    bool ecn_for_plb = ack.ecn_echo();
    if (_plb && !_paths.empty() && _rtt > 0) {
        if (rtt_sample <= _target_rtt && !ecn_for_plb) {
            _plb_last_good = now;
            _plb_interval = (simtime_picosec)(random() % (2 * _rtt)) + 5 * _rtt;
        }
        if (now - _plb_last_good > _plb_interval) {
            _plb_last_good = now;
            uint32_t new_idx = (uint32_t)(random() % _paths.size());
            _path_index = new_idx;
            Route* new_route = _paths[_path_index]->clone();
            new_route->push_back(_sink);
            _route = new_route;
        }
    }

    // Paper Alg.4 lines 6-8: ignore-window early return.
    // After QuickAdapt fires it sets bytes_to_ignore = unacked; all subsequent
    // ACKs are skipped entirely until those in-flight stale signals drain.
    // This blocks QA, FastIncrease, AND core_cases — matching Algorithm 4.
    if (_bytes_ignored < _bytes_to_ignore) {
        send_packets();
        maybe_report_completion();
        return;
    }

    // Paper Alg.4 lines 9-13: QuickAdapt then FastIncrease.
    // Either short-circuits the rest of the loop.
    bool adp = quick_adapt(ack, now);
    bool finc = fast_increase(ack);
    if (adp || finc) {
        clamp_cwnd();
        send_packets();
        maybe_report_completion();
        return;
    }

    // Paper Alg.4 line 14: core_cases (Algorithm 1).
    simtime_picosec trtt = _target_rtt;
    bool ecn = ack.ecn_echo();
    bool healthy = _enable_mcc && message_is_healthy(now);
    uint16_t sz = (new_bytes > 0) ? (uint16_t)std::min((uint64_t)_mtu, new_bytes) : _mtu;

    if (ecn && rtt_sample > trtt) {
        // §III-I.2 Multiplicative Decrease — once per base RTT.
        if (!healthy && (now - _last_decrease) >= _base_rtt) {
            multiplicative_decrease();
        }
    }
    else if (ecn && rtt_sample < trtt) {
        // §III-I.1 Load Balancer Path Change + NO_OP_CC.
        // PLB block above already handles the path change; cwnd is not altered.
    }
    else if (!ecn && rtt_sample > trtt) {
        // §III-I.3 Fair Increase.
        fair_increase(sz);
    }
    else if (!ecn && rtt_sample < trtt) {
        // §III-I.4 Proportional Increase (followed by Fair Increase per §III-I.4).
        proportional_increase(sz, rtt_sample);
        fair_increase(sz);
    }
    // rtt_sample == trtt: no action (paper uses strict < and >).

    clamp_cwnd();
    send_packets();
    maybe_report_completion();
}

// ---------- Sub-procedures --------------------------------------------------

bool
FastflowSrc::quick_adapt(FastflowAck& ack, simtime_picosec now) {
    // Algorithm 2 — at most once per target RTT.
    bool adapted = false;
    if (now < _qa_end && _qa_end != 0) {
        // still inside the previous QA window; just accumulate.
        return false;
    }
    if (_trigger_qa && _qa_end != 0) {
        _trigger_qa = false;
        adapted = true;

        // Paper Alg.2 line 7: cwnd = max(acked, mtu). No scaling constant —
        // the paper §III-E says qa_scaling = 1. RA-QA (Idea 3) substitutes
        // receiver-measured bytes when available.
        uint64_t basis = _acked;
        if (_enable_ra_qa && ack.recv_bytes_trtt() > 0) {
            basis = ack.recv_bytes_trtt();
        }
        _cwnd = (uint32_t)std::max(basis, (uint64_t)_mtu);

        // grace window — ignore stale congestion signals from in-flight pkts
        uint64_t unacked = (_highest_sent > _last_acked)
                              ? (_highest_sent - _last_acked) : 0;
        _bytes_to_ignore = unacked;
        _bytes_ignored   = 0;
        _last_decrease   = now;
    }
    // open the next QA window regardless
    _qa_end = now + _target_rtt;
    _acked = 0;
    return adapted;
}

bool
FastflowSrc::fast_increase(FastflowAck& ack) {
    // Paper Algorithm 3: triggered when rtt ≈ base_rtt (no ECN).
    // Gate: rtt <= base_rtt — strictly matches §III-F "RTT close to base RTT".
    bool clean = !ack.ecn_echo();
    simtime_picosec rtt = _rtt;
    bool rtt_near_base = (rtt == 0) || (rtt <= _base_rtt);

    if (clean && rtt_near_base) {
        uint64_t new_bytes = (ack.ackno() > _last_acked)
                                  ? (ack.ackno() - _last_acked) : _mtu;
        _fast_inc_count += new_bytes;
        if (_fast_inc_count > _cwnd || _in_fast_inc) {
            _cwnd += _k_fastinc * _mtu;
            _in_fast_inc = true;
            return true;
        }
    } else {
        _fast_inc_count = 0;
        _in_fast_inc = false;
    }
    return false;
}

// Paper Eq.1 — Multiplicative Decrease.
// cwnd = cwnd × max(0.5,  1 − (avg_rtt − trtt)/avg_rtt × 0.8)
// Uses _rtt (EWMA of RTT samples, equivalent to paper's avg_rtt).
// Called at most once per base_rtt (enforced by caller).
void
FastflowSrc::multiplicative_decrease() {
    if (_rtt <= _target_rtt) return;   // guard: only when rtt > trtt
    double ratio  = (double)(_rtt - _target_rtt) / (double)_rtt;
    double factor = 1.0 - ratio * 0.8;
    if (factor < 0.5) factor = 0.5;   // cap: never halve more than once
    uint64_t new_cwnd = (uint64_t)((double)_cwnd * factor);
    _cwnd = (uint32_t)std::max(new_cwnd, (uint64_t)_mtu);
    _last_decrease = eventlist().now();
}

// Paper Eq.3 — Fair Increase.
// cwnd += (pkt_size / cwnd) × mtu × fi
void
FastflowSrc::fair_increase(uint16_t pkt_size) {
    double inc = ((double)pkt_size / (double)_cwnd) * (double)_mtu * _fi_const;
    _cwnd += (uint32_t)inc;
}

// Paper Eq.4 — Proportional Increase (PI, followed by FI per §III-I.4).
// cwnd += min(size, (trtt − rtt)/rtt × pkt_size/cwnd × mtu × pi)
// pi = brtt / (trtt − brtt)
void
FastflowSrc::proportional_increase(uint16_t pkt_size, simtime_picosec rtt_sample) {
    if (_target_rtt <= _base_rtt) return;
    double pi  = (double)_base_rtt / (double)(_target_rtt - _base_rtt);
    double inc = ((double)(_target_rtt - rtt_sample) / (double)rtt_sample)
                 * (double)pkt_size / (double)_cwnd
                 * (double)_mtu * pi;
    if (inc > (double)pkt_size) inc = (double)pkt_size;
    if (inc < 0) inc = 0;
    _cwnd += (uint32_t)inc;
}

void
FastflowSrc::clamp_cwnd() {
    // §III-J: maximum window = 1.5 × BDP; minimum = 1 MTU.
    uint32_t hi = (uint32_t)(_bdp_bytes * 1.5);
    if (_cwnd > hi)  _cwnd = hi;
    if (_cwnd < _mtu) _cwnd = _mtu;
}

void
FastflowSrc::update_rtt(simtime_picosec sample) {
    if (sample == 0) return;
    if (_rtt == 0) {
        _rtt  = sample;
        _mdev = sample / 2;
        _rto  = _rtt + 4 * _mdev;
    } else {
        simtime_picosec diff = (sample > _rtt) ? (sample - _rtt) : (_rtt - sample);
        _mdev = (3 * _mdev) / 4 + diff / 4;
        _rtt  = (7 * _rtt) / 8 + sample / 8;
        _rto  = _rtt + 4 * _mdev;
    }
    if (_rto < _min_rto) _rto = _min_rto;
}

void
FastflowSrc::handle_trim(FastflowAck& ack) {
    // Algo 1 lines 28-34, augmented with: (a) a per-seqno dedup so a single
    // dropped packet doesn't get retransmitted twice for the same trim
    // burst, and (b) cwnd-gated retransmissions via _rtx_queue so the
    // recovery traffic itself doesn't storm the bottleneck buffer.
    if (_cwnd > _mtu) _cwnd -= _mtu;
    _trigger_qa = true;
    _drops++;
    simtime_picosec now = eventlist().now();

    FastflowPacket::seq_t sq = ack.trim_seqno();
    if (sq == 0) sq = _last_acked + 1;
    if (sq > _last_acked) {
        if (can_retransmit(sq)) {
            retransmit_seqno(sq);
            _retransmits_dbg++;
        } else {
            _rtx_pending.insert(sq);
            _blocked_rtx_dbg++;
        }
    }
    if (_bytes_ignored >= _bytes_to_ignore) {
        quick_adapt(ack, now);
    }
}

bool
FastflowSrc::can_retransmit(FastflowPacket::seq_t seqno) const {
    auto it = _rtx_blockuntil.find(seqno);
    if (it == _rtx_blockuntil.end()) return true;
    simtime_picosec now = eventlist().now();
    // Strict > so that a feedback ACK arriving exactly at block_until (which
    // is the common case when block_window == RTT) still blocks. Otherwise
    // every retransmit's own trim ACK would re-trigger another retransmit.
    return now > it->second;
}

// Drain seqnos that were trim-acked while their block window was active.
// Called on every ACK arrival so the deferred queue gets a chance at retry
// each time the simulator clock advances — block-windows that expired
// silently otherwise sit idle until the (rare) RTO scan.
void
FastflowSrc::drain_pending_rtx() {
    auto it = _rtx_pending.begin();
    while (it != _rtx_pending.end()) {
        FastflowPacket::seq_t sq = *it;
        if (sq <= _last_acked) {
            it = _rtx_pending.erase(it);
            continue;
        }
        if (can_retransmit(sq)) {
            retransmit_seqno(sq);
            _retransmits_dbg++;
            it = _rtx_pending.erase(it);
        } else {
            ++it;
        }
    }
}

// Block-window dedup state grows once per trim; reclaim entries that have
// either passed their no-retransmit deadline or are below the new cum-ack.
void
FastflowSrc::prune_blockuntil() {
    simtime_picosec now = eventlist().now();
    auto it = _rtx_blockuntil.begin();
    while (it != _rtx_blockuntil.end()) {
        if (it->first <= _last_acked || now >= it->second) {
            it = _rtx_blockuntil.erase(it);
        } else {
            ++it;
        }
    }
}

void
FastflowSrc::retransmit_seqno(FastflowPacket::seq_t seqno) {
    if (seqno <= _last_acked) return;
    if (seqno > _flow_size) return;
    uint32_t sz = std::min((uint32_t)_mtu,
                           (uint32_t)(_flow_size + 1 - seqno));
    FastflowPacket* p = FastflowPacket::newpkt(_flow, *_route, seqno, sz);
    p->set_ts(eventlist().now());
    if (_enable_credits) {
        uint64_t want = _last_acked + (uint64_t)_cwnd + (uint64_t)_mtu;
        p->set_pull_target((want < _flow_size) ? want : _flow_size);
    }
    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);
    p->sendOn();
    _packets_sent += 1;
    // Block window: must exceed one RTT so the trim ACK for *this*
    // retransmit (arriving at ~1 RTT after) does not itself re-fire a
    // retransmit. We use 2 RTT and drain via _rtx_pending — that way
    // retransmits during the block window are not silently lost.
    simtime_picosec block = (_rtt > 0) ? (_rtt * 2) : timeFromUs((uint32_t)1000);
    _rtx_blockuntil[seqno] = eventlist().now() + block;
    if (_RFC2988_RTO_timeout == 0) {
        _RFC2988_RTO_timeout = eventlist().now() + _rto;
    }
}

// ===========================================================================
//                        MCC: message health
// ===========================================================================
bool
FastflowSrc::message_is_healthy(simtime_picosec now) const {
    if (_msg_target_bw <= 0.0) return true;
    if (_msg_start_time == 0)  return true;
    double elapsed_sec = timeAsSec(now - _msg_start_time);
    double expected = elapsed_sec * _msg_target_bw;
    double thresh   = expected * _msg_tolerance;
    return (double)_msg_acked >= thresh;
}

// ===========================================================================
//                        Sending side
// ===========================================================================
bool
FastflowSrc::can_send_one_mtu() const {
    uint64_t inflight = (_highest_sent > _last_acked)
                         ? (_highest_sent - _last_acked) : 0;

    if (_enable_credits) {
        // Soft ceiling: 2×cwnd + mtu.  cwnd tracks credit_rate × RTT so this
        // bounds inflight without hard blocking after QA events (where cwnd
        // temporarily falls below inflight).  Actual sends are credit-gated.
        uint64_t soft_ceil = (uint64_t)_cwnd * 2 + _mtu;
        if (inflight + _mtu > soft_ceil) return false;
        if (_initial_burst_remaining >= _mtu) return true;
        return (_receiver_credits >= _mtu);
    }
    return (inflight + _mtu <= _cwnd);
}

int
FastflowSrc::send_packets() {
    if (!_established) return 0;
    int count = 0;
    while (_highest_sent + _mtu <= _flow_size + _mtu) {
        if (!can_send_one_mtu()) break;
        if (!send_next_packet()) break;
        count++;
    }
    // In credit mode: if we couldn't send due to missing credits but have
    // unsent data, send an RTS to update the receiver's pull_target.  This
    // prevents the deadlock that arises after QA drops cwnd below the last
    // issued credit count (pull_target retreated, pacer went idle).
    if (_enable_credits && count == 0 && _highest_sent < _flow_size) {
        maybe_send_rts();
    }
    if (_highest_sent < _flow_size && _RFC2988_RTO_timeout == 0) {
        _RFC2988_RTO_timeout = eventlist().now() + _rto;
    }
    return count;
}

void
FastflowSrc::maybe_send_rts() {
    if (!_sink || !_route) return;
    // Rate-limit to once per base_rtt to avoid flooding the network.
    simtime_picosec now = eventlist().now();
    if (_last_rts_time > 0 && now - _last_rts_time < _base_rtt) return;
    _last_rts_time = now;

    // Use EQDS-style so the pacer gets the same credit horizon as regular data
    // sends.  The primary recovery mechanism for QA stalls is the capped RTO
    // (100µs max in credit mode) which fires retransmits and advances last_acked,
    // eventually pushing the EQDS pull_target past credits_issued.
    uint64_t want = _last_acked + (uint64_t)_cwnd + (uint64_t)_mtu;
    uint64_t pt = (want < _flow_size) ? want : _flow_size;
    FastflowPacket* rts = FastflowPacket::new_rts_pkt(_flow, *_route, pt);
    rts->flow().logTraffic(*rts, *this, TrafficLogger::PKT_CREATESEND);
    rts->sendOn();
}

bool
FastflowSrc::send_next_packet() {
    if (_highest_sent >= _flow_size) return false;
    uint32_t sz = std::min((uint32_t)_mtu,
                           (uint32_t)(_flow_size - _highest_sent));

    FastflowPacket* p = FastflowPacket::newpkt(_flow, *_route,
                                               _highest_sent + 1, sz);
    p->set_ts(eventlist().now());
    // EQDS-style pull_target: last_acked + cwnd + mtu.
    // Limits outstanding credits to cwnd bytes ahead of the cumulative ack —
    // QA/FD/MD reduce cwnd → pull_target shrinks → pacer slows → rate drops.
    // Prevents the burst storm that arises when FI grows cwnd to BDP while
    // highest_sent + BDP pre-issues the full BDP in credits.
    // The monotonic sink update keeps the pacer running across QA events;
    // the RTS uses highest_sent + BDP to guarantee backlog > 0 on restart.
    if (_enable_credits) {
        uint64_t want = _last_acked + (uint64_t)_cwnd + (uint64_t)_mtu;
        p->set_pull_target((want < _flow_size) ? want : _flow_size);
    } else {
        p->set_pull_target(0);
    }
    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);
    _highest_sent += sz;
    _packets_sent += 1;

    if (_enable_credits) {
        if (_initial_burst_remaining >= sz) {
            _initial_burst_remaining -= sz;
        } else if (_receiver_credits >= sz) {
            _receiver_credits -= sz;
        }
    }

    p->sendOn();
    return true;
}

void
FastflowSrc::retransmit_packet() {
    // Legacy entry point — defer to the dedup-aware path so callers don't
    // accidentally storm the queue.
    if (_last_acked >= _flow_size) return;
    if (can_retransmit(_last_acked + 1)) {
        retransmit_seqno(_last_acked + 1);
    }
}

// ===========================================================================
//                        Packet receive (ACK / pull)
// ===========================================================================
void
FastflowSrc::receivePacket(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_RCVDESTROY);

    // Detect packet type by attempted downcast.
    FastflowPull* pull = dynamic_cast<FastflowPull*>(&pkt);
    if (pull) {
        // Track credit interarrival time (EMA) as a proxy for N competing
        // senders.  credit_ema ≈ pkt_time × N so cwnd_max = credit_rate × RTT
        // = (MTU / credit_ema) × RTT = BDP / N — naturally scales with load.
        simtime_picosec now = eventlist().now();
        if (_last_credit_time > 0) {
            simtime_picosec interval = now - _last_credit_time;
            _credit_ema = (_credit_ema == 0) ? interval
                        : (3 * _credit_ema + interval) / 4;
        }
        _last_credit_time = now;

        _receiver_credits += pull->credit_bytes();
        pull->free();
        if (_receiver_credits >= _mtu) {
            send_packets();
        }
        return;
    }
    FastflowAck* ack = dynamic_cast<FastflowAck*>(&pkt);
    if (!ack) {
        pkt.free();
        return;
    }
    handle_ack(*ack);
    ack->free();
}

// ===========================================================================
//                        Retransmission timer
// ===========================================================================
void
FastflowSrc::rtx_timer_hook(simtime_picosec now, simtime_picosec period) {
    if (!_established) return;
    if (_RFC2988_RTO_timeout == 0) return;
    if (now < _RFC2988_RTO_timeout) return;
    if (_highest_sent == 0) return;
    if (_rtx_timeout_pending) return;
    _rtx_timeout_pending = true;
    _rto *= 2;
    // In credit mode, cap at 100µs so RTO-based credit recovery (from QA-
    // induced stalls) completes in < 10ms.  Without this cap, exponential
    // backoff stretches to seconds, permanently stalling trigger-chained flows.
    simtime_picosec max_rto = _enable_credits ? timeFromUs((uint32_t)100) : timeFromMs(1000);
    if (_rto > max_rto) _rto = max_rto;
    _RFC2988_RTO_timeout = now + _rto;
    eventlist().sourceIsPendingRel(*this, 0);  // fire ASAP
}

// ===========================================================================
//                        Completion
// ===========================================================================
void
FastflowSrc::maybe_report_completion() {
    if (_flow_finished) return;
    if (_last_acked < _flow_size) return;
    _flow_finished = true;
    _rtx_scanner->deregisterSrc(*this);
    if (_sink && _sink->_coflow_id != NO_COFLOW) {
        CoflowRegistry::instance().mark_finished(_sink->_coflow_id, _sink->_sender_id);
    }
    simtime_picosec fct = eventlist().now() - _start_time;
    // Same line format as Swift so plot_swift_results.py parses it natively.
    cout << "FCT " << _name
         << " start_us "  << timeAsUs(_start_time)
         << " finish_us " << timeAsUs(eventlist().now())
         << " fct_us "    << timeAsUs(fct)
         << " size_bytes " << _flow_size
         << endl;
    if (_end_trigger) _end_trigger->activate();
}

// ===========================================================================
//                        FastflowSink
// ===========================================================================
FastflowSink::FastflowSink()
    : Logged("fastflow_sink"),
      _src(NULL), _route_back(NULL),
      _cumulative_ack(0), _drops(0),
      _bytes_received_window(0), _bytes_received_last_window(0), _window_start(0),
      _pacer(NULL), _pull_target(0), _credits_issued(0),
      _in_active_queue(false), _in_rtx_queue(false),
      _coflow_id(NO_COFLOW), _sender_id(0),
      _bytes_received_total(0)
{
    _nodename = "fastflow_sink";
}

void
FastflowSink::connect(FastflowSrc& src, const Route& routeback) {
    _src = &src;
    Route* rt = routeback.clone();
    rt->push_back(&src);
    _route_back = rt;
}

void
FastflowSink::set_coflow(CoflowId cid, SenderId sid) {
    _coflow_id = cid;
    _sender_id = sid;
    if (cid != NO_COFLOW) {
        CoflowRegistry::instance().register_sender(cid, sid);
    }
}

void
FastflowSink::update_pull_target(uint64_t new_pt) {
    // Bidirectional: tracks last_acked + cwnd (EQDS-style from data sends).
    // When QA reduces cwnd, pull_target shrinks — pacer throttles credit rate.
    // The RTS (sent with highest_sent + BDP + MTU) always has backlog > 0
    // relative to the bidirectional pull_target, restarting the pacer after
    // QA-induced stalls without the permanent deadlock that monotonic causes.
    _pull_target = new_pt;
}

void
FastflowSink::update_recv_window(uint32_t pkt_size, simtime_picosec now) {
    if (_window_start == 0) _window_start = now;
    if (now - _window_start >= FastflowSrc::_target_rtt) {
        // Save the completed window before resetting so ACKs report a stable,
        // non-zero value even if the new window has just started.
        _bytes_received_last_window = _bytes_received_window;
        _bytes_received_window = 0;
        _window_start = now;
    }
    _bytes_received_window += pkt_size;
}

void
FastflowSink::receivePacket(Packet& pkt) {
    simtime_picosec now = _src ? _src->eventlist().now() : 0;
    FastflowPacket* p = dynamic_cast<FastflowPacket*>(&pkt);
    if (!p) {
        pkt.free();
        return;
    }

    // RTS (credit-request): sender is credit-blocked and wants new credits.
    // Update pull_target (may decrease if cwnd was cut by QA) and kick the
    // pacer if there is backlog.
    if (p->is_rts()) {
        if (p->pull_target() > 0) {
            update_pull_target(p->pull_target());
            if (_pacer && backlog() > 0 && !_in_active_queue && !_in_rtx_queue) {
                _in_active_queue = true;
                _pacer->request_active(this);
            }
        }
        p->free();
        return;
    }

    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_RCVDESTROY);

    // Trimmed packet: header arrived, payload was stripped en route.
    if (pkt.header_only()) {
        _drops++;
        send_ack(*p, /*trimmed=*/true, now);
        // RTX priority: jump to front of pacer queue so the retransmit's
        // follow-on new data gets credit ahead of non-retransmitting senders.
        if (_pacer && !_in_rtx_queue) {
            _in_rtx_queue = true;
            _in_active_queue = false;
            _pacer->request_rtx(this);
        }
        p->free();
        return;
    }

    uint32_t sz = p->size();
    update_recv_window(sz, now);
    _bytes_received_total += sz;

    // Coflow progress update (Idea 2)
    if (_coflow_id != NO_COFLOW) {
        CoflowRegistry::instance().update_progress(_coflow_id, _sender_id, sz);
    }

    // Track cumulative ACK
    FastflowPacket::seq_t seqno = p->seqno();
    if (seqno == _cumulative_ack + 1) {
        _cumulative_ack = seqno + sz - 1;
        while (!_received.empty() && _received.front() == _cumulative_ack + 1) {
            _received.pop_front();
            _cumulative_ack += sz;
        }
    } else if (seqno < _cumulative_ack + 1) {
        // duplicate / out of order before cum_ack — ignore
    } else {
        if (_received.empty() || seqno > _received.back()) {
            _received.push_back(seqno);
        } else {
            for (auto it = _received.begin(); it != _received.end(); ++it) {
                if (seqno == *it) break;
                if (seqno < *it) { _received.insert(it, seqno); break; }
            }
        }
    }

    send_ack(*p, /*trimmed=*/false, now);
    p->free();
}

void
FastflowSink::send_ack(FastflowPacket& pkt, bool trimmed, simtime_picosec now) {
    bool ecn = (pkt.flags() & ECN_CE) != 0;
    FastflowPacket::seq_t trim_sq = trimmed ? pkt.seqno() : 0;
    uint64_t ra_qa_bytes = (_bytes_received_last_window > 0)
                               ? _bytes_received_last_window
                               : _bytes_received_window;
    FastflowAck* ack = FastflowAck::newpkt(pkt.flow(), *_route_back,
                                           _cumulative_ack,
                                           trim_sq,
                                           pkt.ts(),
                                           ecn,
                                           trimmed,
                                           ra_qa_bytes);
    ack->flow().logTraffic(*ack, *this, TrafficLogger::PKT_CREATESEND);
    ack->sendOn();

    // For normal (non-trimmed) data packets: update pull target from the
    // sender's advertised cwnd and request credit if there is backlog.
    // Trimmed packets are handled in receivePacket (RTX queue path).
    if (!trimmed && _pacer && pkt.pull_target() > 0) {
        update_pull_target(pkt.pull_target());
        if (backlog() > 0 && !_in_active_queue && !_in_rtx_queue) {
            _in_active_queue = true;
            _pacer->request_active(this);
        }
    }
}

void
FastflowSink::send_pull_credit() {
    if (!_route_back || !_src) return;
    _in_active_queue = false;
    _in_rtx_queue    = false;

    uint64_t grant = std::min((uint64_t)FastflowSrc::_mtu, backlog());
    if (grant == 0) return;

    _credits_issued += grant;
    FastflowPull* pull = FastflowPull::newpkt(_src->flow(), *_route_back,
                                              (uint32_t)grant);
    pull->sendOn();
}

// ===========================================================================
//                        FastflowPullPacer
// ===========================================================================
FastflowPullPacer::FastflowPullPacer(linkspeed_bps linkspeed, uint16_t mtu,
                                     EventList& el)
    : EventSource(el, "fastflow_pull_pacer"), _running(false)
{
    // Time to transmit one MTU at 99% of link speed (mirrors EqdsPullPacer).
    _pkt_time = (simtime_picosec)(0.99 * (double)mtu * 8.0
                                  / (double)linkspeed * 1e12);
}

bool
FastflowPullPacer::in_rtx_queue(FastflowSink* s) const {
    for (auto* p : _rtx_queue) if (p == s) return true;
    return false;
}

bool
FastflowPullPacer::in_active_queue(FastflowSink* s) const {
    for (auto* p : _active_queue) if (p == s) return true;
    return false;
}

void
FastflowPullPacer::request_rtx(FastflowSink* sink) {
    if (in_rtx_queue(sink)) return;
    _rtx_queue.push_back(sink);
    if (!_running) {
        _running = true;
        eventlist().sourceIsPendingRel(*this, 0);
    }
}

void
FastflowPullPacer::request_active(FastflowSink* sink) {
    if (in_active_queue(sink)) return;
    _active_queue.push_back(sink);
    if (!_running) {
        _running = true;
        eventlist().sourceIsPendingRel(*this, 0);
    }
}

void
FastflowPullPacer::doNextEvent() {
    FastflowSink* sink = NULL;

    if (!_rtx_queue.empty()) {
        sink = _rtx_queue.front();
        _rtx_queue.pop_front();
        sink->send_pull_credit();
        // Re-enqueue in active if it still has backlog after the credit grant.
        if (sink->backlog() > 0 && !in_active_queue(sink)) {
            sink->_in_active_queue = true;
            _active_queue.push_back(sink);
        }
    } else if (!_active_queue.empty()) {
        sink = _active_queue.front();
        _active_queue.pop_front();
        sink->send_pull_credit();
        if (sink->backlog() > 0 && !in_active_queue(sink)) {
            sink->_in_active_queue = true;
            _active_queue.push_back(sink);
        }
    } else {
        _running = false;
        return;
    }

    eventlist().sourceIsPendingRel(*this, _pkt_time);
}

// ===========================================================================
//                        FastflowPacer
// ===========================================================================
FastflowPacer::FastflowPacer(FastflowSrc& src, EventList& eventlist)
    : EventSource(eventlist, "fastflow_pacer"),
      _src(&src), _interpacket_delay(0),
      _last_send(eventlist.now()), _next_send(0) {}

void
FastflowPacer::schedule_send(simtime_picosec delay) {
    _interpacket_delay = delay;
    _next_send = _last_send + delay;
    if (_next_send <= eventlist().now()) {
        _next_send = eventlist().now();
        doNextEvent();
        return;
    }
    eventlist().sourceIsPending(*this, _next_send);
}

void
FastflowPacer::cancel() {
    _interpacket_delay = 0;
    _next_send = 0;
    eventlist().cancelPendingSource(*this);
}

void
FastflowPacer::just_sent() {
    _last_send = eventlist().now();
}

void
FastflowPacer::doNextEvent() {
    _src->send_next_packet();
    _last_send = eventlist().now();
    if (_src->_pacing_delay > 0) {
        schedule_send(_src->_pacing_delay);
    } else {
        _interpacket_delay = 0;
        _next_send = 0;
    }
}

// ===========================================================================
//                        FastflowRtxTimerScanner
// ===========================================================================
FastflowRtxTimerScanner::FastflowRtxTimerScanner(simtime_picosec scanPeriod,
                                                 EventList& eventlist)
    : EventSource(eventlist, "fastflow_rtx_scanner"), _scanPeriod(scanPeriod) {
    eventlist.sourceIsPendingRel(*this, _scanPeriod);
}

void
FastflowRtxTimerScanner::registerSrc(FastflowSrc& src) {
    _srcs.push_back(&src);
}

void
FastflowRtxTimerScanner::deregisterSrc(FastflowSrc& src) {
    _srcs.remove(&src);
}

void
FastflowRtxTimerScanner::doNextEvent() {
    simtime_picosec now = eventlist().now();
    for (auto* src : _srcs) {
        src->rtx_timer_hook(now, _scanPeriod);
    }
    eventlist().sourceIsPendingRel(*this, _scanPeriod);
}
