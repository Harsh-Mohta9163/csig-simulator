// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "coflow_state.h"

void
CoflowRegistry::register_sender(CoflowId cid, SenderId sid) {
    if (cid == NO_COFLOW) return;
    CoflowEntry& e = _coflows[cid];
    if (e.bytes_by_sender.find(sid) == e.bytes_by_sender.end()) {
        e.bytes_by_sender[sid] = 0;
        e.num_senders++;
    }
}

void
CoflowRegistry::update_progress(CoflowId cid, SenderId sid, uint32_t bytes) {
    if (cid == NO_COFLOW) return;
    CoflowEntry& e = _coflows[cid];
    uint64_t& s = e.bytes_by_sender[sid];
    s += bytes;
    if (s > e.max_progress) e.max_progress = s;
}

uint32_t
CoflowRegistry::credit_for(CoflowId cid, SenderId sid,
                           uint32_t fair_quantum,
                           uint32_t priority_quantum) const {
    if (_policy == COFLOW_OFF || cid == NO_COFLOW) {
        return fair_quantum;
    }
    auto it = _coflows.find(cid);
    if (it == _coflows.end()) return fair_quantum;
    const CoflowEntry& e = it->second;
    auto sit = e.bytes_by_sender.find(sid);
    if (sit == e.bytes_by_sender.end()) return fair_quantum;
    uint64_t my = sit->second;

    // Does *any* sender lag by > gap_threshold ?
    bool has_straggler = false;
    for (const auto& kv : e.bytes_by_sender) {
        if (e.max_progress - kv.second > _gap_threshold) {
            has_straggler = true;
            break;
        }
    }

    if (my == e.max_progress && has_straggler && e.num_senders > 1) {
        // Leader: starve until stragglers catch up.
        return 0;
    }
    if (e.max_progress - my > _gap_threshold) {
        // Straggler: flood with priority credits.
        return priority_quantum;
    }
    // Middle of the pack: fair share.
    return fair_quantum;
}
