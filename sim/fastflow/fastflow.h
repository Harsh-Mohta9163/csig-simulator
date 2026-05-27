// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// FASTFLOW: sender-based CC using ECN + RTT + packet trimming.
// Implements the algorithm from Bonato et al. 2024 (arXiv 2404.01630),
// reproduced and extended for this codebase.
//
// Layered features (all togglable via static flags so a single source/sink
// class covers every variant the paper plan needs):
//   - FASTFLOW core: paper Algorithm 1 (FD/MD/FI/MI), Algorithm 2 (QuickAdapt),
//                    Algorithm 3 (FastIncrease), Sec 3.5.1 (Wait-to-Decrease).
//   - Idea 1: receiver credits + Message-Level CC overrides.
//   - Idea 2: coflow-aware asymmetric credit allocation (state on the sink).
//   - Idea 3: Receiver-Anchored QuickAdapt — sink stamps its own
//             bytes-received-per-trtt onto each ACK, sender uses that
//             directly when QuickAdapt fires.

#ifndef FASTFLOW_H
#define FASTFLOW_H

#include <list>
#include <map>
#include <set>
#include <vector>
#include <string>

#include "config.h"
#include "network.h"
#include "eventlist.h"
#include "loggertypes.h"
#include "trigger.h"

#include "fastflowpacket.h"
#include "coflow_state.h"

class FastflowSink;
class FastflowSrc;
class FastflowRtxTimerScanner;
class FastflowPacer;

// ===========================================================================
// FastflowPacer — runs the source in paced mode when cwnd drops below 1 MTU.
// ===========================================================================
class FastflowPacer : public EventSource {
public:
    FastflowPacer(FastflowSrc& src, EventList& eventlist);
    bool is_pending() const { return _interpacket_delay > 0; }
    void schedule_send(simtime_picosec delay);
    void cancel();
    void just_sent();
    virtual void doNextEvent();

private:
    FastflowSrc* _src;
    simtime_picosec _interpacket_delay;
    simtime_picosec _last_send;
    simtime_picosec _next_send;
};

// ===========================================================================
// FastflowSrc — the sender.
// ===========================================================================
class FastflowSrc : public EventSource, public PacketSink, public TriggerTarget {
    friend class FastflowSink;
    friend class FastflowPacer;
    friend class FastflowRtxTimerScanner;

public:
    FastflowSrc(FastflowRtxTimerScanner& rtx_scanner,
                TrafficLogger* pktlogger, EventList& eventlist);

    void connect(const Route& routeout, const Route& routeback,
                 FastflowSink& sink, simtime_picosec starttime);
    virtual void receivePacket(Packet& pkt);
    virtual void doNextEvent();
    virtual const string& nodename() { return _nodename; }
    PacketFlow& flow() { return _flow; }

    // Trigger interface
    virtual void activate();
    void set_end_trigger(Trigger& trigger);

    // Workload config
    void set_flowsize(uint64_t bytes);
    uint64_t flowsize() const { return _flow_size; }
    void set_paths(vector<const Route*>* rt);
    void enable_plb() { _plb = true; }

    // ====================== Global tunables (paper Sec 3.5) ================
    static uint16_t _mtu;          // MTU in bytes (data packet payload)
    static double   _fd_const;     // Fair decrease constant (0.8)
    static double   _fi_const;     // Fair increase constant (0.25 at 100Gbps ref)
    static double   _md_const;     // Multiplicative decrease constant (2; 4 if no trim)
    static double   _qa_scaling;   // QuickAdapt window scale (0.8)
    static double   _wtd_alpha;    // EMA constant for WTD (0.125)
    static double   _wtd_thresh;   // WTD trigger threshold (0.25)
    static uint32_t _k_fastinc;    // packets per FastIncrease step (2)
    static simtime_picosec _base_rtt;     // base RTT for the topology
    static simtime_picosec _target_rtt;   // 1.5 * base_rtt (paper default)
    static uint32_t _bdp_bytes;    // BDP in bytes for cwnd clamp

    // ====================== Feature toggles (CLI-driven) ===================
    static bool _enable_ra_qa;     // Idea 3: receiver-anchored QuickAdapt
    static bool _enable_credits;   // Idea 1: gate sends on receiver credits
    static bool _enable_mcc;       // Idea 1: message-level CC override
    static bool _trim_supported;   // if false, double _md and rely on timeouts

    // For workload-aware MCC: target bandwidth per flow (bytes/sec)
    void set_msg_target_bw(double bytes_per_sec) { _msg_target_bw = bytes_per_sec; }
    void set_msg_tolerance(double t)             { _msg_tolerance = t; }
    bool message_is_healthy(simtime_picosec now) const;

    // Stats
    uint64_t packets_sent() const { return _packets_sent; }
    uint64_t bytes_acked()  const { return _last_acked; }
    uint64_t retransmits()  const { return _retransmits_dbg; }
    uint64_t blocked_rtx()  const { return _blocked_rtx_dbg; }

    // ====================== Internal accessors for debugging ==============
    uint32_t cwnd() const { return _cwnd; }
    simtime_picosec rtt() const { return _rtt; }
    uint32_t drops() const { return _drops; }

protected:
    // --- Connectivity ---
    bool _established;
    const Route* _route;          // outbound route (sender -> sink)
    FastflowSink* _sink;
    PacketFlow _flow;
    vector<const Route*> _paths;  // ECMP path pool
    uint32_t _path_index;
    bool _plb;                          // PLB enabled
    simtime_picosec _plb_last_good;     // timestamp of last congestion-free ACK
    simtime_picosec _plb_interval;      // retry-path interval (randomised)

    // --- Sending state ---
    uint64_t _highest_sent;       // highest seqno ever sent (bytes)
    uint64_t _last_acked;         // cumulative ack point
    uint64_t _flow_size;          // total bytes to send
    uint64_t _packets_sent;       // counter for stats

    // --- Core FASTFLOW state (paper Algo 1) ---
    uint32_t _cwnd;               // congestion window (bytes)
    uint64_t _acked;              // bytes acked in current QuickAdapt window
    uint64_t _bytes_ignored;      // accumulator for post-QA grace
    uint64_t _bytes_to_ignore;    // ignore-budget after QuickAdapt fires
    bool     _trigger_qa;         // a trim/timeout has armed QuickAdapt
    double   _avg_wtd;            // EMA of ECN markings for Wait-to-Decrease
    uint32_t _fast_inc_count;     // bytes of clean ACKs in current FI window
    bool     _in_fast_inc;        // FastIncrease currently active
    simtime_picosec _qa_end;      // earliest time a new QuickAdapt may fire
    simtime_picosec _last_decrease;

    // --- RTT/RTO ---
    simtime_picosec _rtt, _rto, _mdev;
    simtime_picosec _min_rto;
    simtime_picosec _RFC2988_RTO_timeout;
    bool _rtx_timeout_pending;
    uint32_t _retransmit_cnt;
    uint32_t _drops;

    // --- Retransmission dedup + deferred retry ---
    // Trim ACKs retransmit the trimmed seqno immediately (paper Algo 1).
    // _rtx_blockuntil holds (seqno -> earliest-next-retransmit-time) so a
    // single dropped packet that gets trimmed several times in quick
    // succession only triggers one retransmit per RTT. When a trim arrives
    // for a seqno whose block has not expired we stash it in _rtx_pending
    // and drain it on the next event whose timestamp clears the block —
    // otherwise blocked trims would be silently lost between block-expiry
    // and the next RTO fire (which happens at a much coarser cadence).
    std::map<FastflowPacket::seq_t, simtime_picosec> _rtx_blockuntil;
    std::set<FastflowPacket::seq_t> _rtx_pending;
    uint64_t _retransmits_dbg;
    uint64_t _blocked_rtx_dbg;

    // --- Idea 1: credits + MCC ---
    uint64_t _initial_burst_remaining;  // 1 BDP blind send before credits
    uint64_t _receiver_credits;         // bytes the sink has authorised
    simtime_picosec _msg_start_time;
    uint64_t _msg_acked;                // bytes acked since msg start
    double   _msg_target_bw;            // bytes/sec the message expects
    double   _msg_tolerance;            // healthy if acked >= elapsed*bw*tol

    // --- FCT bookkeeping ---
    simtime_picosec _start_time;
    bool _flow_finished;
    Trigger* _end_trigger;

    // Pacer (used when cwnd < 1 MTU)
    FastflowPacer _pacer;
    simtime_picosec _pacing_delay;

    // Logging
    TrafficLogger* _traffic_logger;
    string _nodename;
    FastflowRtxTimerScanner* _rtx_scanner;

private:
    // --- Control loop (Algorithm 1) ---
    void handle_ack(FastflowAck& ack);
    bool wait_to_decrease(bool ecn, simtime_picosec now);
    bool quick_adapt(FastflowAck& ack, simtime_picosec now);
    bool fast_increase(FastflowAck& ack);
    void fair_decrease(uint16_t pkt_size);
    void multiplicative_decrease(uint16_t pkt_size, simtime_picosec rtt_sample);
    void fair_increase(uint16_t pkt_size);
    void multiplicative_increase(uint16_t pkt_size, simtime_picosec rtt_sample);
    void clamp_cwnd();
    void update_rtt(simtime_picosec sample);

    // --- Sending ---
    int  send_packets();
    bool send_next_packet();
    void retransmit_packet();
    void send_syn();
    bool can_send_one_mtu() const;     // checks credits + cwnd

    // --- Trim / timeout reaction (Algo 1 lines 28-34) ---
    void handle_trim(FastflowAck& ack);
    void retransmit_seqno(FastflowPacket::seq_t seqno);
    void drain_pending_rtx();
    void prune_blockuntil();
    bool can_retransmit(FastflowPacket::seq_t seqno) const;
    void rtx_timer_hook(simtime_picosec now, simtime_picosec period);

    // --- Completion ---
    void maybe_report_completion();

    void startflow();
};

// ===========================================================================
// FastflowSink — the receiver.
// ===========================================================================
class FastflowSink : public PacketSink, public Logged {
    friend class FastflowSrc;

public:
    FastflowSink();
    virtual void receivePacket(Packet& pkt);
    virtual const string& nodename() { return _nodename; }
    void connect(FastflowSrc& src, const Route& routeback);
    virtual void setName(const string& name) { Logged::setName(name); _nodename = name; }

    // Idea 2: register this sink as part of a coflow.
    void set_coflow(CoflowId cid, SenderId sid);

    // Idea 1: credit-grant emission (called periodically when credits enabled).
    void enable_credit_mode(linkspeed_bps host_linkspeed);
    void issue_credit_if_due(simtime_picosec now);

    uint64_t cumulative_ack() const { return _cumulative_ack; }
    uint32_t drops()          const { return _drops; }

protected:
    FastflowSrc* _src;
    const Route* _route_back;
    string _nodename;

    // ACK state
    uint64_t _cumulative_ack;          // last contiguous byte
    std::list<FastflowPacket::seq_t> _received;  // out-of-order holes
    uint32_t _drops;

    // RA-QA window
    uint64_t _bytes_received_window;       // current (possibly partial) window
    uint64_t _bytes_received_last_window;  // previous completed window (stable)
    simtime_picosec _window_start;

    // Credit mode (Idea 1)
    bool _credit_mode;
    linkspeed_bps _host_linkspeed;
    simtime_picosec _next_credit_time;
    uint32_t _credit_quantum;          // default ~1 MTU per RTT/N

    // Coflow membership (Idea 2)
    CoflowId _coflow_id;
    SenderId _sender_id;
    uint64_t _bytes_received_total;    // for coflow progress tracking

private:
    void send_ack(FastflowPacket& pkt, bool trimmed, simtime_picosec now);
    void update_recv_window(uint32_t pkt_size, simtime_picosec now);
};

// ===========================================================================
// FastflowRtxTimerScanner — periodic scan to fire RTOs.
// ===========================================================================
class FastflowRtxTimerScanner : public EventSource {
public:
    FastflowRtxTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    virtual void doNextEvent();
    void registerSrc(FastflowSrc& src);
    void deregisterSrc(FastflowSrc& src);

private:
    simtime_picosec _scanPeriod;
    std::list<FastflowSrc*> _srcs;
};

#endif // FASTFLOW_H
