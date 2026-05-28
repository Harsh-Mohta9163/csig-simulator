// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// FASTFLOW packets: data + ack + (optional) credit.
//
// We piggy-back on Packet's existing typesystem (SWIFT for data, SWIFTACK for
// ACK). Queues/pipes care about size and headers, not enum identity, so this
// avoids touching network.h. Class identity carries the FASTFLOW-specific
// fields the sender/sink need at receive time.
//
// FastflowAck carries four signals from the sink back to the source:
//   - cumulative ackno
//   - echoed timestamp (for RTT measurement)
//   - ECN echo bit  (was the data packet ECN-marked)
//   - trimmed bit   (was the data packet header-stripped en route)
// and one optional field for the Receiver-Anchored QuickAdapt extension:
//   - recv_bytes_trtt: bytes the sink has seen in its last trtt window
//
// FastflowPull is a tiny credit-grant packet used only when running in
// FASTFLOW+EQDS hybrid mode (Idea 1). Vanilla FASTFLOW never sees one.

#ifndef FASTFLOW_PACKET_H
#define FASTFLOW_PACKET_H

#include "network.h"

class FastflowPacket : public Packet {
public:
    typedef uint64_t seq_t;

    inline static FastflowPacket* newpkt(PacketFlow& flow, const Route& route,
                                         seq_t seqno, int size) {
        FastflowPacket* p = _packetdb.allocPacket();
        p->set_route(flow, route, size, seqno + size - 1);
        p->_type = SWIFT;
        p->_seqno = seqno;
        p->_syn = false;
        p->_ts = 0;
        p->_pull_target = 0;
        return p;
    }

    inline static FastflowPacket* new_syn_pkt(PacketFlow& flow, const Route& route,
                                              seq_t seqno, int size) {
        FastflowPacket* p = newpkt(flow, route, seqno, size);
        p->_syn = true;
        return p;
    }

    void free() { _packetdb.freePacket(this); }
    virtual ~FastflowPacket() {}

    inline seq_t seqno() const { return _seqno; }
    inline simtime_picosec ts() const { return _ts; }
    inline void set_ts(simtime_picosec ts) { _ts = ts; }
    inline bool is_syn() const { return _syn; }
    inline uint64_t pull_target() const { return _pull_target; }
    inline void set_pull_target(uint64_t pt) { _pull_target = pt; }
    virtual PktPriority priority() const { return Packet::PRIO_LO; }

protected:
    seq_t _seqno;
    bool _syn;
    simtime_picosec _ts;
    uint64_t _pull_target;  // sender's current cwnd (0 = credits disabled)
    static PacketDB<FastflowPacket> _packetdb;
};

class FastflowAck : public Packet {
public:
    typedef FastflowPacket::seq_t seq_t;

    inline static FastflowAck* newpkt(PacketFlow& flow, const Route& route,
                                      seq_t ackno, seq_t trim_seqno,
                                      simtime_picosec ts_echo,
                                      bool ecn_echo, bool trimmed,
                                      uint64_t recv_bytes_trtt = 0) {
        FastflowAck* p = _packetdb.allocPacket();
        p->set_route(flow, route, ACKSIZE, ackno);
        p->_type = SWIFTACK;
        p->_ackno = ackno;
        p->_trim_seqno = trim_seqno;
        p->_ts_echo = ts_echo;
        p->_ecn_echo = ecn_echo;
        p->_is_trimmed = trimmed;
        p->_recv_bytes_trtt = recv_bytes_trtt;
        return p;
    }

    void free() { _packetdb.freePacket(this); }
    virtual ~FastflowAck() {}

    inline seq_t ackno() const { return _ackno; }
    inline seq_t trim_seqno() const { return _trim_seqno; }
    inline simtime_picosec ts_echo() const { return _ts_echo; }
    inline bool ecn_echo() const { return _ecn_echo; }
    inline bool is_trimmed() const { return _is_trimmed; }
    inline uint64_t recv_bytes_trtt() const { return _recv_bytes_trtt; }
    inline void set_trimmed(bool t) { _is_trimmed = t; }
    inline void set_ecn_echo(bool e) { _ecn_echo = e; }
    inline void set_recv_bytes_trtt(uint64_t b) { _recv_bytes_trtt = b; }
    virtual PktPriority priority() const { return Packet::PRIO_HI; }

    const static int ACKSIZE = 40;

protected:
    seq_t _ackno;
    seq_t _trim_seqno;
    simtime_picosec _ts_echo;
    bool _ecn_echo;
    bool _is_trimmed;
    uint64_t _recv_bytes_trtt;
    static PacketDB<FastflowAck> _packetdb;
};

// Pull/credit packet for FASTFLOW+EQDS hybrid (Idea 1).
class FastflowPull : public Packet {
public:
    inline static FastflowPull* newpkt(PacketFlow& flow, const Route& route,
                                       uint32_t credit_bytes) {
        FastflowPull* p = _packetdb.allocPacket();
        p->set_route(flow, route, PULLSIZE, 0);
        p->_type = SWIFTACK;
        p->_credit_bytes = credit_bytes;
        return p;
    }

    void free() { _packetdb.freePacket(this); }
    virtual ~FastflowPull() {}

    inline uint32_t credit_bytes() const { return _credit_bytes; }
    virtual PktPriority priority() const { return Packet::PRIO_HI; }

    const static int PULLSIZE = 32;

protected:
    uint32_t _credit_bytes;
    static PacketDB<FastflowPull> _packetdb;
};

#endif
