// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// Entry point for the FASTFLOW simulator.
//
// Modes (selected by -mode <name>):
//   fastflow             : vanilla sender-based FASTFLOW (default).
//   fastflow+eqds        : FASTFLOW sender + EQDS-style receiver credits.
//   fastflow+eqds+mcc    : above + Message-Level CC overrides.
//   fastflow+eqds+mcc+coflow : above + asymmetric coflow credit shaping.
//
// Independently toggleable:
//   -ra_qa on|off        : Receiver-Anchored QuickAdapt (Idea 3).
//   -trimming on|off     : enable packet trimming (CompositeQueue, default on).
//
// CLI parity with main_swift.cpp is kept where possible so plot scripts and
// connection-matrix files remain interchangeable across protocols.

#include <iostream>
#include <list>
#include <map>
#include <math.h>
#include <sstream>
#include <string.h>

#include "config.h"
#include "eventlist.h"
#include "network.h"
#include "pipe.h"
#include "logfile.h"
#include "loggers.h"
#include "clock.h"
#include "compositequeue.h"
#include "topology.h"
#include "connection_matrix.h"
#include "fat_tree_topology.h"
#include "main.h"
#include "trigger.h"

#include "fastflow/fastflow.h"
#include "fastflow/coflow_state.h"

#define DEFAULT_NODES   128
#define DEFAULT_QUEUE_SIZE 500   // packets

EventList eventlist;
Logfile* lg;

static void exit_error(const char* progname) {
    cout << "Usage: " << progname
         << " [-tm tm-file] [-topo topo-file] [-nodes N] [-q Q] [-mtu MTU]"
         << " [-linkspeed Mbps] [-end us]"
         << " [-trimming on|off] [-plb on|off] [-ra_qa on|off]"
         << " [-mode fastflow|fastflow+eqds|fastflow+eqds+mcc|fastflow+eqds+mcc+coflow]"
         << " [-msg_target_bw Bps] [-msg_tolerance 0..1]"
         << " [-coflow_gap bytes]"
         << endl;
    exit(1);
}

static bool flag_on(const char* s) {
    return strcmp(s, "on") == 0 || strcmp(s, "true") == 0 || strcmp(s, "1") == 0;
}

int main(int argc, char** argv) {
    Clock c(timeFromSec(5.0 / 100.0), eventlist);

    uint32_t no_of_nodes = DEFAULT_NODES;
    mem_b queuesize = DEFAULT_QUEUE_SIZE;
    linkspeed_bps linkspeed = speedFromMbps((double)HOST_NIC);
    uint32_t packet_size = 4000;
    simtime_picosec endtime = timeFromMs(1.2);

    bool trimming = true;
    bool ra_qa = false;
    bool plb = true;  // paper: multi-pathing enabled for all algorithms
    bool blast_start = false;
    bool use_credits = false;
    bool use_mcc = false;
    bool use_coflow = false;
    double fi_scale = 1.0;

    double msg_target_bw = 0.0;
    double msg_tolerance = 0.85;
    uint64_t coflow_gap_bytes = 4ULL * 1024 * 1024;

    const char* tm_file   = NULL;
    const char* topo_file = NULL;

    stringstream logfile_name(ios_base::out);
    logfile_name << "logout.dat";

    // ----- CLI parsing --------------------------------------------------
    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "-o"))            { logfile_name.str(""); logfile_name << argv[++i]; }
        else if (!strcmp(argv[i], "-nodes"))   { no_of_nodes = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-tm"))      { tm_file = argv[++i]; }
        else if (!strcmp(argv[i], "-topo"))    { topo_file = argv[++i]; }
        else if (!strcmp(argv[i], "-q"))       { queuesize = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-mtu"))     { packet_size = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-linkspeed")) { linkspeed = speedFromMbps(atof(argv[++i])); }
        else if (!strcmp(argv[i], "-end"))     { endtime = timeFromUs(atof(argv[++i])); }
        else if (!strcmp(argv[i], "-trimming"))    { trimming = flag_on(argv[++i]); }
        else if (!strcmp(argv[i], "-plb"))         { plb = flag_on(argv[++i]); }
        else if (!strcmp(argv[i], "-ra_qa"))       { ra_qa = flag_on(argv[++i]); }
        else if (!strcmp(argv[i], "-blast_start")) { blast_start = flag_on(argv[++i]); }
        else if (!strcmp(argv[i], "-reps"))        { FastflowSrc::_reps_enabled = flag_on(argv[++i]); }
        else if (!strcmp(argv[i], "-fi_scale"))    { fi_scale = atof(argv[++i]); }
        else if (!strcmp(argv[i], "-mode")) {
            const char* m = argv[++i];
            if (!strcmp(m, "fastflow")) {
                use_credits = false; use_mcc = false; use_coflow = false;
            } else if (!strcmp(m, "fastflow+eqds")) {
                use_credits = true;  use_mcc = false; use_coflow = false;
            } else if (!strcmp(m, "fastflow+eqds+mcc")) {
                use_credits = true;  use_mcc = true;  use_coflow = false;
            } else if (!strcmp(m, "fastflow+eqds+mcc+coflow")) {
                use_credits = true;  use_mcc = true;  use_coflow = true;
            } else {
                cerr << "Unknown mode '" << m << "'\n"; exit_error(argv[0]);
            }
        }
        else if (!strcmp(argv[i], "-msg_target_bw")) { msg_target_bw = atof(argv[++i]); }
        else if (!strcmp(argv[i], "-msg_tolerance")) { msg_tolerance = atof(argv[++i]); }
        else if (!strcmp(argv[i], "-coflow_gap"))    { coflow_gap_bytes = strtoull(argv[++i], NULL, 10); }
        else { exit_error(argv[0]); }
        i++;
    }

    Packet::set_packet_size(packet_size);
    eventlist.setEndtime(endtime);
    queuesize = queuesize * Packet::data_packet_size();
    srand(13);

    // ----- Wire FASTFLOW global toggles --------------------------------
    FastflowSrc::_mtu = packet_size;
    FastflowSrc::_enable_ra_qa = ra_qa;
    FastflowSrc::_enable_credits = use_credits;
    FastflowSrc::_enable_mcc = use_mcc;
    FastflowSrc::_trim_supported = trimming;
    FastflowSrc::_blast_start = blast_start;
    // MD formula is now fully multiplicative (paper Eq.1); _md_const removed.

    CoflowRegistry::instance().reset();
    if (use_coflow) {
        CoflowRegistry::instance().set_policy(COFLOW_ASYMMETRIC);
        CoflowRegistry::instance().set_gap_threshold(coflow_gap_bytes);
    }

    cout << "[fastflow] mode "
         << (use_credits ? (use_mcc ? (use_coflow ? "fastflow+eqds+mcc+coflow"
                                                  : "fastflow+eqds+mcc")
                                    : "fastflow+eqds")
                         : "fastflow")
         << " trimming=" << trimming
         << " plb="      << plb
         << " ra_qa="    << ra_qa
         << " mtu="      << packet_size
         << " nodes="    << no_of_nodes
         << " linkspeed=" << (linkspeed / 1000000) << " Mbps"
         << endl;

    Logfile logfile(logfile_name.str(), eventlist);
    lg = &logfile;
    logfile.setStartTime(timeFromSec(0));

    // ----- Topology -----------------------------------------------------
    FatTreeTopology* top;
    queue_type net_qt    = trimming ? COMPOSITE : RANDOM;
    queue_type sender_qt = FAIR_PRIO;
    if (topo_file) {
        top = FatTreeTopology::load(topo_file, NULL, eventlist, queuesize,
                                    net_qt, sender_qt);
    } else {
        top = new FatTreeTopology(no_of_nodes, linkspeed, queuesize,
                                  NULL, &eventlist, NULL, net_qt, sender_qt, 0);
    }
    no_of_nodes = top->no_of_nodes();
    cout << "[fastflow] actual nodes " << no_of_nodes << endl;

    // ----- Derive FASTFLOW parameters from the topology -----------------
    //
    // Per paper Sec 3.5:
    //   base_rtt is the empty-network RTT (2 * link_latency * hops + queuing-free
    //     switch traversal). At 800Gbps with hop_latency ~600ns and 3 tier
    //     traversal, we approximate base_rtt = 12us. For correctness we set
    //     it from queuesize/linkspeed-derived BDP.
    //   target_rtt = 1.5 * base_rtt
    //   fi, mi scaled by gamma = bdp / reference_bdp (100Gbps,12us)
    //
    // We use 12us as the reference because the FASTFLOW paper uses that.

    simtime_picosec brtt = timeFromUs((uint32_t)12);
    simtime_picosec trtt = (brtt * 3) / 2;
    uint64_t bdp_bytes = (uint64_t)((double)linkspeed * timeAsSec(brtt) / 8.0);
    uint64_t reference_bdp = (uint64_t)((double)speedFromMbps(100000.0) * timeAsSec(timeFromUs((uint32_t)12)) / 8.0);
    double gamma = (double)bdp_bytes / (double)reference_bdp;

    FastflowSrc::_base_rtt   = brtt;
    FastflowSrc::_target_rtt = trtt;
    FastflowSrc::_bdp_bytes  = (uint32_t)bdp_bytes;
    // fi scaled by gamma = bdp/reference_bdp so ramp-up time is topology-invariant.
    // fi_scale CLI flag allows experimenting with stronger Fair Increase bias
    // for incast fairness (paper §III-I.3).
    FastflowSrc::_fi_const   = 0.25 * gamma * fi_scale;
    // pi constant is brtt/(trtt-brtt), computed per-call inside proportional_increase.
    FastflowSrc::_k_fastinc  = 2;

    cout << "[fastflow] brtt=" << timeAsUs(brtt) << "us trtt=" << timeAsUs(trtt) << "us"
         << " bdp=" << bdp_bytes << "B gamma=" << gamma << endl;

    // ----- Connection matrix --------------------------------------------
    ConnectionMatrix* conns = new ConnectionMatrix(no_of_nodes);
    if (tm_file) {
        cout << "[fastflow] loading traffic matrix " << tm_file << endl;
        if (!conns->load(tm_file)) exit(-1);
    } else {
        cout << "[fastflow] reading traffic matrix from stdin" << endl;
        conns->load(cin);
    }
    if (conns->N != no_of_nodes) {
        cerr << "[fastflow] tm nodes " << conns->N
             << " != topology nodes " << no_of_nodes << endl;
        exit(-1);
    }

    // ----- Flow construction --------------------------------------------
    // RTO scan period must be much shorter than typical RTO so a stuck
    // hole (e.g. because the trim ACK was truly dropped in the fabric)
    // gets recovered within a few RTTs — not after 10ms.
    FastflowRtxTimerScanner rtx_scanner(timeFromUs((uint32_t)50), eventlist);

    vector<connection*>* all_conns = conns->getAllConnections();
    vector<const Route*>*** net_paths = new vector<const Route*>**[no_of_nodes];
    for (uint32_t a = 0; a < no_of_nodes; a++) {
        net_paths[a] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t b = 0; b < no_of_nodes; b++) net_paths[a][b] = NULL;
    }

    // One pull pacer per destination node, shared across all senders targeting
    // that node. This is the key to incast serialisation: N senders to the
    // same destination share one pacer, so each gets credit at rate/N.
    std::map<uint32_t, FastflowPullPacer*> dst_pacers;

    list<FastflowSrc*> srcs;
    list<FastflowSink*> sinks;

    uint32_t connID = 0;
    for (uint32_t k = 0; k < all_conns->size(); k++) {
        connection* crt = all_conns->at(k);
        uint32_t s = crt->src, d = crt->dst;
        connID++;

        if (!net_paths[s][d]) net_paths[s][d] = top->get_paths(s, d);
        if (!net_paths[d][s]) net_paths[d][s] = top->get_paths(d, s);

        // Pick a path via ECMP-style random hashing (deterministic seed above).
        uint32_t choice = rand() % net_paths[s][d]->size();

        Route* routeout = new Route(*(net_paths[s][d]->at(choice)));
        Route* routein  = new Route(*(net_paths[d][s]->at(choice)));

        FastflowSrc*  src  = new FastflowSrc(rtx_scanner, NULL, eventlist);
        FastflowSink* sink = new FastflowSink();

        // Give the src all ECMP paths so PLB can switch between them at runtime.
        src->set_paths(net_paths[s][d]);
        if (plb) src->enable_plb();

        src->setName ("fastflow_"      + ntoa(s) + "_" + ntoa(d));
        sink->setName("fastflow_sink_" + ntoa(s) + "_" + ntoa(d));
        logfile.writeName(*src);
        logfile.writeName(*sink);

        if (crt->size > 0) src->set_flowsize((uint64_t)crt->size);

        if (use_mcc) {
            if (msg_target_bw <= 0.0) {
                // Default: assume each flow ideally gets the full link speed
                // (gives a hawkish-but-realistic baseline for "is the message
                // on schedule?"). Operator can override with -msg_target_bw.
                msg_target_bw = (double)linkspeed / 8.0;
            }
            src->set_msg_target_bw(msg_target_bw);
            src->set_msg_tolerance(msg_tolerance);
        }

        if (use_credits) {
            if (!dst_pacers.count(d)) {
                dst_pacers[d] = new FastflowPullPacer(linkspeed, packet_size, eventlist);
            }
            sink->set_pacer(dst_pacers[d]);
        }

        // Coflow membership: by convention use crt->recv_done_trigger as the
        // coflow id (a non-zero value means this connection participates).
        // The connection-matrix file can be extended later with a dedicated
        // 'coflow N' token; for now this lets workloads declare coflow
        // membership using the existing trigger field.
        CoflowId cid = NO_COFLOW;
        if (use_coflow && crt->recv_done_trigger != 0) {
            cid = (CoflowId)crt->recv_done_trigger;
        }
        sink->set_coflow(cid, (SenderId)s);

        srcs.push_back(src);
        sinks.push_back(sink);

        simtime_picosec starttime = crt->trigger ? TRIGGER_START
                                                 : timeFromUs((uint32_t)crt->start);
        src->connect(*routeout, *routein, *sink, starttime);

        if (crt->trigger) {
            Trigger* trig = conns->getTrigger(crt->trigger, eventlist);
            trig->add_target(*src);
        }
        if (crt->send_done_trigger) {
            Trigger* trig = conns->getTrigger(crt->send_done_trigger, eventlist);
            src->set_end_trigger(*trig);
        }
    }

    cout << "[fastflow] loaded " << connID << " connections" << endl;

    // ----- Run ---------------------------------------------------------
    while (eventlist.doNextEvent()) { /* spin */ }

    cout << "[fastflow] done. simulated to "
         << timeAsUs(eventlist.now()) << "us" << endl;

    // If any flow didn't finish, dump per-source state so we can debug stalls.
    int unfinished = 0;
    for (auto* src : srcs) {
        if (src->bytes_acked() < src->flowsize()) {
            if (unfinished < 10) {
                cout << "[fastflow] UNFINISHED " << src->nodename()
                     << " sent=" << src->packets_sent()
                     << " acked=" << src->bytes_acked()
                     << " flow=" << src->flowsize()
                     << " cwnd=" << src->cwnd()
                     << " rtt_us=" << timeAsUs(src->rtt())
                     << " drops=" << src->drops()
                     << " rtx=" << src->retransmits()
                     << " blocked=" << src->blocked_rtx()
                     << endl;
            }
            unfinished++;
        }
    }
    cout << "[fastflow] unfinished=" << unfinished << "/" << srcs.size() << endl;
    return 0;
}
