// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "coflow_state.h"

void
CoflowRegistry::register_sender(CoflowId cid, SenderId sid) {
    if (cid == NO_COFLOW) return;
    CoflowEntry& e = _coflows[cid];
    if (e.bytes_by_sender.find(sid) == e.bytes_by_sender.end()) {
        e.bytes_by_sender[sid] = 0;
        e.num_senders++;
        e.flow_count[sid] = 0;
    }
    e.flow_count[sid]++;
}

void
CoflowRegistry::update_progress(CoflowId cid, SenderId sid, uint32_t bytes) {
    if (cid == NO_COFLOW) return;
    CoflowEntry& e = _coflows[cid];
    uint64_t& s = e.bytes_by_sender[sid];
    s += bytes;
    if (s > e.max_progress) e.max_progress = s;
}

void
CoflowRegistry::mark_finished(CoflowId cid, SenderId sid) {
    if (cid == NO_COFLOW) return;
    auto it = _coflows.find(cid);
    if (it == _coflows.end()) return;
    CoflowEntry& e = it->second;
    auto fc = e.flow_count.find(sid);
    if (fc == e.flow_count.end()) return;
    if (fc->second > 0) fc->second--;
    // Only mark as fully finished when all flows from this sender are done.
    if (fc->second == 0) {
        e.finished[sid] = true;
    }
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

    // Skip if this sender is already done — no credits needed.
    auto fit = e.finished.find(sid);
    if (fit != e.finished.end() && fit->second) return fair_quantum;

    uint64_t my = sit->second;

    // Compute max_progress among ACTIVE (not-finished) senders only.
    uint64_t active_max = 0;
    for (const auto& kv : e.bytes_by_sender) {
        auto fi = e.finished.find(kv.first);
        if (fi != e.finished.end() && fi->second) continue;  // skip finished
        if (kv.second > active_max) active_max = kv.second;
    }

    if (active_max - my > _gap_threshold) {
        // Straggler: flood with priority credits to help it catch up.
        return priority_quantum;
    }
    // Leader or middle of the pack: fair share.
    // (We do NOT starve leaders — in a credit-gated fabric, starvation causes
    // deadlocks and only the straggler boost matters for coflow completion time.)
    return fair_quantum;
}
