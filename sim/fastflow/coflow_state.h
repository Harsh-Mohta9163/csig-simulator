// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// CoflowRegistry — abstract receiver-side state for "asymmetric credit
// allocation in a SmartNIC" (Idea 2 in the plan).
//
// We model the policy at the receiver only; SRAM/cycle costs are not
// charged. The registry is a singleton accessed by every FastflowSink that
// belongs to a coflow. On every data packet a sink updates its own
// progress; when emitting a credit it asks the registry which class
// (leader / median / straggler) it is in and gets a credit quantum back.
//
// The policy:
//   leader     — bytes_received == max_progress AND a straggler exists
//                  -> grant 0 credits (starve so spine queues drain)
//   straggler  — max_progress - my_progress > gap_threshold
//                  -> grant priority_credit_bytes
//   middle     — otherwise
//                  -> grant standard_credit_bytes
//
// Coflow membership is identified by an explicit `coflow <id>` token in
// the .cm connection-matrix file, parsed by ConnectionMatrix.

#ifndef FASTFLOW_COFLOW_STATE_H
#define FASTFLOW_COFLOW_STATE_H

#include <stdint.h>
#include <unordered_map>

typedef uint32_t CoflowId;
typedef uint32_t SenderId;

#define NO_COFLOW ((CoflowId)0xffffffff)

struct CoflowEntry {
    uint64_t max_progress;
    std::unordered_map<SenderId, uint64_t> bytes_by_sender;
    std::unordered_map<SenderId, bool> finished;      // true once ALL flows done
    std::unordered_map<SenderId, uint32_t> flow_count; // flows registered per sender
    uint32_t num_senders;
    CoflowEntry() : max_progress(0), num_senders(0) {}
};

enum CoflowPolicy {
    COFLOW_OFF = 0,        // disable asymmetric allocation
    COFLOW_ASYMMETRIC = 1  // leader-starve / straggler-flood
};

class CoflowRegistry {
public:
    static CoflowRegistry& instance() {
        static CoflowRegistry singleton;
        return singleton;
    }

    void reset() {
        _coflows.clear();
        _policy = COFLOW_OFF;
    }

    void set_policy(CoflowPolicy p) { _policy = p; }
    void set_gap_threshold(uint64_t bytes) { _gap_threshold = bytes; }
    void set_priority_factor(double f) { _priority_factor = f; }
    CoflowPolicy policy() const { return _policy; }
    uint64_t gap_threshold() const { return _gap_threshold; }

    // Sinks call this on every data packet they receive.
    void update_progress(CoflowId cid, SenderId sid, uint32_t bytes);

    // Called when issuing a credit. Returns the credit quantum
    // appropriate for this sender's coflow role. fair_quantum and
    // priority_quantum are caller-provided in bytes.
    uint32_t credit_for(CoflowId cid, SenderId sid,
                        uint32_t fair_quantum,
                        uint32_t priority_quantum) const;

    // Mark a sender as done so it doesn't count as a straggler.
    void mark_finished(CoflowId cid, SenderId sid);

    bool has_coflow(CoflowId cid) const {
        return _coflows.find(cid) != _coflows.end();
    }

    void register_sender(CoflowId cid, SenderId sid);

private:
    CoflowRegistry() : _policy(COFLOW_OFF),
                       _gap_threshold(4ULL * 1024 * 1024),
                       _priority_factor(2.0) {}

    std::unordered_map<CoflowId, CoflowEntry> _coflows;
    CoflowPolicy _policy;
    uint64_t _gap_threshold;
    double _priority_factor;
};

#endif
