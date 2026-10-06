// The protocol model: the MESI protocol of rtl/l1d.sv and rtl/bus.sv,
// written as a state machine over abstract caches, one bus and memory.
//
// It is used twice:
//   * model/mc.cpp explores every reachable state for small configurations
//     (2-3 caches, 1-2 line addresses) and checks the invariants below;
//   * sim/smp_main.cpp (--model) steps it with every coherence event the RTL
//     produces and checks that the model allows the event and ends in the
//     same line states (mesi::Agreement, at the end of this file).
//
// What is modelled (per cache): the MESI state of each line, whether the
// copy holds the latest value ("fresh": data are abstracted to fresh/stale,
// which is exact for this purpose - a store makes its copy the only fresh
// one, copying data copies freshness), one outstanding request (address,
// waiting core operation, bus command), the one-entry write-back buffer,
// the LR reservation (plus a ghost bit: no other cache wrote the line since
// the LR) and the LR lock-out flag.  The bus holds one transaction: owner,
// command, address, the caches that still have to answer the snoop, and
// whether a cache supplied the line (and that line's freshness).
//
// Actions (each atomic):
//   Access(c, a, op[, victim])  the core of c issues a load/store/LR/SC/AMO:
//                               a hit is performed at once; an SC without
//                               reservation fails at once; a miss records the
//                               request (BusRd, BusRdX or BusUpgr) and may
//                               evict one other valid line (a dirty one goes
//                               to the write-back buffer)
//   Grant(c)                    the bus takes c's request
//   GrantWB(c)                  the bus writes c's write-back buffer to memory
//   Snoop(d)                    cache d answers the current transaction
//   Complete(c)                 every snooper has answered: c installs the line
//                               and performs its operation in the same step
//   Timeout(c)                  c's LR lock-out window closes
// Invariants checked after every action: single writer / multiple readers
// (at most one E or M copy, and then no S copy), data value (every valid
// copy, the write-back buffer and data on the bus are fresh; memory is fresh
// when no cache holds the line dirty), every load returns the latest value,
// and an SC succeeds only if no other cache wrote the line since the LR.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace mesi {

enum { I = 0, S = 1, E = 2, M = 3 };
enum { RD = 0, RDX = 1, UPGR = 2, WB = 3 };
enum { LD = 0, ST = 1, LR = 2, SC = 3, AMO = 4 };
enum Bug { NONE = 0, LOST_UPGRADE = 1, WB_NO_SNOOP = 2, LRSC_NO_CLEAR = 3, E_IGNORES_SHARED = 4, LOCKOUT_FOREVER = 5 };
enum Viol {
    OK = 0,
    V_SWMR,          // two writable copies, or a writable copy next to a readable one
    V_DATA,          // a valid copy, buffer, bus data or memory that should be current is stale
    V_STALE_READ,    // a load/LR/AMO returned a stale value
    V_SC_ATOMIC,     // an SC succeeded after another cache wrote the line
    V_NOT_ENABLED,   // (Agreement only) the RTL did something the model does not allow
};

static inline const char *viol_name(int v)
{
    static const char *n[] = {"ok", "single-writer/multiple-reader violated", "data-value invariant violated",
                              "a read returned a stale value", "SC succeeded after a remote write",
                              "event not allowed by the model"};
    return n[v];
}
static inline const char *st_str(int s) { static const char *n[4] = {"I", "S", "E", "M"}; return n[s & 3]; }
static inline const char *cmd_str(int c) { static const char *n[4] = {"BusRd", "BusRdX", "BusUpgr", "BusWB"}; return n[c & 3]; }
static inline const char *op_str(int o) { static const char *n[5] = {"load", "store", "lr", "sc", "amo"}; return n[o]; }
static inline bool exclusive_op(int op) { return op != LD; }

struct Cfg {
    int n = 2, a = 1, bug = NONE;
    bool track_data = true;     // false in the agreement check (data are checked by sim/memcheck.h)
};

struct Line { uint8_t st = I, fresh = 0; };

template <int MA> struct State {
    static const int MAXN = 4;
    Line line[MAXN][MA];
    uint8_t memf[MA];
    // per cache
    uint8_t pend[MAXN], pa[MAXN], pop[MAXN], pcmd[MAXN];
    uint8_t wb[MAXN], wa[MAXN], wf[MAXN];
    uint8_t resv[MAXN], ra[MAXN], intact[MAXN], young[MAXN];
    // bus
    uint8_t busy, owner, bcmd, ba, need, sup, supf, shared;
    State() { std::memset(this, 0, sizeof *this); for (int x = 0; x < MA; x++) memf[x] = 1; }
};

// ------------------------------------------------------------- invariants
template <int MA> int check_line(const State<MA> &s, const Cfg &k, int a)
{
    int ex = 0, sh = 0, dirty = 0;
    for (int c = 0; c < k.n; c++) {
        int st = s.line[c][a].st;
        if (st == E || st == M) ex++;
        if (st == S) sh++;
        if (st == M) dirty = 1;
    }
    if (ex > 1 || (ex == 1 && sh > 0)) return V_SWMR;
    if (!k.track_data) return OK;
    for (int c = 0; c < k.n; c++) {
        if (s.line[c][a].st != I && !s.line[c][a].fresh) return V_DATA;
        if (s.wb[c] && s.wa[c] == a) {
            if (!s.wf[c]) return V_DATA;
            dirty = 1;
        }
    }
    bool inflight = s.busy && s.sup && s.ba == a;
    if (inflight && !s.supf) return V_DATA;
    if (!dirty && !inflight && !s.memf[a]) return V_DATA;
    return OK;
}

template <int MA> int check(const State<MA> &s, const Cfg &k)
{
    for (int a = 0; a < k.a; a++) {
        int v = check_line(s, k, a);
        if (v) return v;
    }
    return OK;
}

// ---------------------------------------------------------------- actions
// a write by cache c to line a: every other copy of the value becomes stale
template <int MA> void write_line(State<MA> &s, const Cfg &k, int c, int a)
{
    s.line[c][a].st = M;
    s.line[c][a].fresh = 1;
    for (int d = 0; d < k.n; d++) {
        if (d == c) continue;
        s.line[d][a].fresh = 0;
        if (s.wb[d] && s.wa[d] == a) s.wf[d] = 0;
        if (s.resv[d] && s.ra[d] == a) s.intact[d] = 0;
    }
    s.memf[a] = 0;
    if (s.busy && s.sup && s.ba == a) s.supf = 0;
}

// perform core operation op of cache c on line a (the line is present with
// enough permission); returns a violation or OK; *sc_ok gets an SC's outcome
template <int MA> int perform(State<MA> &s, const Cfg &k, int c, int a, int op, int *sc_ok = nullptr)
{
    int v = OK;
    bool reads = op == LD || op == LR || op == AMO;
    if (reads && k.track_data && !s.line[c][a].fresh) v = V_STALE_READ;
    switch (op) {
    case ST: case AMO: write_line(s, k, c, a); break;
    case LR:
        s.resv[c] = 1; s.ra[c] = (uint8_t)a; s.intact[c] = 1; s.young[c] = 1;
        break;
    case SC: {
        bool ok = s.resv[c] && s.ra[c] == a;
        if (sc_ok) *sc_ok = ok;
        if (ok && !s.intact[c] && v == OK) v = V_SC_ATOMIC;
        if (ok) write_line(s, k, c, a);
        s.resv[c] = 0; s.young[c] = 0; s.intact[c] = 0;
        break;
    }
    default: break;
    }
    return v;
}

enum AccessKind { A_HIT, A_SCFAIL, A_MISS };

template <int MA> int classify(const State<MA> &s, int c, int a, int op)
{
    if (op == SC && !(s.resv[c] && s.ra[c] == a)) return A_SCFAIL;
    int st = s.line[c][a].st;
    if (st != I && (!exclusive_op(op) || st == E || st == M)) return A_HIT;
    return A_MISS;
}

template <int MA> bool can_access(const State<MA> &s, int c, int a, int op)
{
    if (s.pend[c]) return false;
    return classify(s, c, a, op) != A_MISS || !s.wb[c];
}

// victim: -1 none, else another valid line of c
template <int MA> int access(State<MA> &s, const Cfg &k, int c, int a, int op, int victim, int *sc_ok = nullptr)
{
    int kind = classify(s, c, a, op);
    if (kind == A_SCFAIL) {
        if (sc_ok) *sc_ok = 0;
        s.resv[c] = 0; s.young[c] = 0; s.intact[c] = 0;
        return OK;
    }
    if (kind == A_HIT) {
        int v = perform(s, k, c, a, op, sc_ok);
        return v ? v : check_line(s, k, a);
    }
    s.young[c] = 0;   // the core waits for the cache: no lock-out
    s.pend[c] = 1; s.pa[c] = (uint8_t)a; s.pop[c] = (uint8_t)op;
    if (s.line[c][a].st == S) {
        s.pcmd[c] = UPGR;
    } else {
        s.pcmd[c] = op == LD ? RD : RDX;
        if (victim >= 0) {
            Line &l = s.line[c][victim];
            if (l.st == M) { s.wb[c] = 1; s.wa[c] = (uint8_t)victim; s.wf[c] = l.fresh; }
            l.st = I;
            if (s.resv[c] && s.ra[c] == victim) { s.resv[c] = 0; s.intact[c] = 0; }
            int v = check_line(s, k, victim);
            if (v) return v;
        }
    }
    return check_line(s, k, a);
}

template <int MA> bool can_grant(const State<MA> &s, int c) { return !s.busy && s.pend[c]; }
template <int MA> void grant(State<MA> &s, const Cfg &k, int c)
{
    s.busy = 1; s.owner = (uint8_t)c; s.bcmd = s.pcmd[c]; s.ba = s.pa[c];
    s.need = (uint8_t)(((1u << k.n) - 1) & ~(1u << c));
    s.sup = 0; s.supf = 0; s.shared = 0;
}

template <int MA> bool can_grant_wb(const State<MA> &s, int c) { return !s.busy && !s.pend[c] && s.wb[c]; }
template <int MA> int grant_wb(State<MA> &s, const Cfg &k, int c)
{
    int a = s.wa[c];
    s.memf[a] = s.wf[c];
    s.wb[c] = 0;
    return check_line(s, k, a);
}

template <int MA> bool deferred(const State<MA> &s, int d)
{
    return s.young[d] && s.resv[d] && s.ra[d] == s.ba && !s.pend[d];
}
template <int MA> bool can_snoop(const State<MA> &s, int d)
{
    return s.busy && ((s.need >> d) & 1) && !deferred(s, d);
}

// *supplied: 1 from the array, 2 from the write-back buffer
template <int MA> int snoop(State<MA> &s, const Cfg &k, int d, int *supplied = nullptr)
{
    int a = s.ba, cmd = s.bcmd, sp = 0;
    Line &l = s.line[d][a];
    if (cmd == RD) {
        if (l.st == M) { s.sup = 1; s.supf = l.fresh; sp = 1; }
        if (l.st != I) { s.shared = 1; l.st = S; }
        if (s.wb[d] && s.wa[d] == a && k.bug != WB_NO_SNOOP) { s.sup = 1; s.supf = s.wf[d]; s.wb[d] = 0; sp = 2; }
    } else {
        if (l.st == M && cmd == RDX) { s.sup = 1; s.supf = l.fresh; sp = 1; }
        l.st = I;
        if (cmd == RDX && s.wb[d] && s.wa[d] == a && k.bug != WB_NO_SNOOP) {
            s.sup = 1; s.supf = s.wf[d]; s.wb[d] = 0; sp = 2;
        }
        if (s.resv[d] && s.ra[d] == a && k.bug != LRSC_NO_CLEAR) { s.resv[d] = 0; s.young[d] = 0; s.intact[d] = 0; }
        if (s.pend[d] && s.pa[d] == a && s.pcmd[d] == UPGR && k.bug != LOST_UPGRADE) s.pcmd[d] = RDX;
    }
    if (supplied) *supplied = sp;
    s.need &= (uint8_t)~(1u << d);
    return check_line(s, k, a);
}

template <int MA> bool can_complete(const State<MA> &s) { return s.busy && s.need == 0; }
template <int MA> int complete(State<MA> &s, const Cfg &k, int *sc_ok = nullptr)
{
    int c = s.owner, a = s.ba, cmd = s.bcmd;
    uint8_t f = cmd == UPGR ? s.line[c][a].fresh : s.sup ? s.supf : s.memf[a];
    if (cmd == RD && s.sup) s.memf[a] = s.supf;   // the dirty line is flushed to memory
    bool shared = s.shared && k.bug != E_IGNORES_SHARED;
    s.line[c][a].st = cmd == RD ? (shared ? S : E) : M;
    s.line[c][a].fresh = f;
    s.busy = 0; s.need = 0; s.sup = 0; s.supf = 0; s.shared = 0;
    s.pend[c] = 0;
    int v = perform(s, k, c, a, s.pop[c], sc_ok);
    if (v) return v;
    return check_line(s, k, a);
}

template <int MA> bool can_timeout(const State<MA> &s, const Cfg &k, int c)
{
    return s.young[c] && k.bug != LOCKOUT_FOREVER;
}
template <int MA> void timeout(State<MA> &s, int c) { s.young[c] = 0; }

// Progress: can the protocol alone (grants, snoop answers, completions,
// write-backs, lock-out timeouts - no new core accesses) bring the system to
// a state with no request, no buffered write-back and an idle bus?  The only
// thing a snoop answer can wait for is the lock-out, so the greedy schedule
// below is exact.  Returns true if it can.
template <int MA> bool drains(State<MA> s, const Cfg &k)
{
    Cfg q = k;
    q.track_data = false;
    for (int step = 0; step < 64; step++) {
        if (s.busy) {
            if (can_complete(s)) { complete(s, q); continue; }
            bool moved = false;
            for (int d = 0; d < k.n && !moved; d++)
                if (can_snoop(s, d)) { snoop(s, q, d); moved = true; }
            for (int d = 0; d < k.n && !moved; d++)
                if (((s.need >> d) & 1) && can_timeout(s, k, d)) { timeout(s, d); moved = true; }
            if (!moved) return false;
            continue;
        }
        bool moved = false;
        for (int c = 0; c < k.n && !moved; c++)
            if (can_grant(s, c)) { grant(s, q, c); moved = true; }
        for (int c = 0; c < k.n && !moved; c++)
            if (can_grant_wb(s, c)) { grant_wb(s, q, c); moved = true; }
        if (!moved) return true;
    }
    return false;
}

// ===================================================================
// Agreement: step the model with the RTL's coherence events.
//
// Line addresses are mapped to model addresses as they appear.  For every
// event the model must allow the corresponding action, start from the line
// state the RTL reports and end in the state the RTL reaches.  Events of one
// cycle are applied in an order the hardware guarantees is consistent:
// snoop answers, then fills, then accesses and misses, then bus grants.
// ===================================================================
#ifdef MESI_AGREEMENT
const int AGREE_LINES = 2048;

struct Agreement {
    Cfg k;
    State<AGREE_LINES> *s;
    std::unordered_map<uint32_t, int> map;
    uint64_t checked = 0;
    int nlines = 0;

    Agreement(int n, int bug) : s(new State<AGREE_LINES>())
    {
        k.n = n; k.a = 0; k.bug = bug; k.track_data = false;
    }
    ~Agreement() { delete s; }

    int idx(uint32_t line, std::string *err)
    {
        auto it = map.find(line >> 4);
        if (it != map.end()) return it->second;
        if (nlines >= AGREE_LINES) {
            if (err) *err = "more distinct lines than the agreement model holds";
            return -1;
        }
        map[line >> 4] = nlines;
        k.a = nlines + 1;
        return nlines++;
    }

    bool fail(std::string *err, const char *fmt, int h, uint32_t line, const char *more = "")
    {
        if (err) {
            char b[300];
            std::snprintf(b, sizeof b, fmt, h, line, more);
            *err = b;
        }
        return false;
    }

    static int rtl_op(int kind) { return kind; }   // same encoding (trace.h K_* == mesi LD..AMO)

    void log(FILE *f, uint64_t cyc, int h, const char *ev, uint32_t line, const char *fmt_extra)
    {
        if (f) std::fprintf(f, "{\"c\":%llu,\"h\":%d,\"e\":\"%s\",\"line\":\"%08x\"%s}\n", (unsigned long long)cyc, h, ev,
                            line, fmt_extra);
    }

    template <class T, class B>
    bool cycle(uint64_t cyc, const std::vector<T> &tr, const B &bt, FILE *f, std::string *err)
    {
        char x[200];
        // 1. snoop answers
        for (int h = 0; h < k.n; h++) {
            const T &t = tr[(size_t)h];
            if (!t.snp || t.snp_cmd > 2) continue;
            std::snprintf(x, sizeof x, ",\"cmd\":\"%s\",\"from\":\"%s\",\"to\":\"%s\",\"supply\":%d,\"wb\":%d", cmd_str(t.snp_cmd),
                          st_str(t.snp_old), st_str(t.snp_new), t.supply_arr || t.supply_wb, (int)t.supply_wb);
            log(f, cyc, h, "snoop", t.snp_line, x);
            if (!err) continue;
            checked++;
            int a = idx(t.snp_line, err);
            if (a < 0) return false;
            if (!s->busy || s->ba != a || s->bcmd != t.snp_cmd || !((s->need >> h) & 1))
                return fail(err, "hart %d answered a snoop for line %08x the model's bus is not asking for%s", h, t.snp_line);
            if (deferred(*s, h)) {
                if (!can_timeout(*s, k, h))
                    return fail(err, "hart %d answered a snoop for %08x inside its LR lock-out window%s", h, t.snp_line);
                timeout(*s, h);
            }
            if (s->line[h][a].st != t.snp_old)
                return fail(err, "hart %d: snooped line %08x is %s in the RTL", h, t.snp_line, st_str(t.snp_old));
            int sp = 0;
            int v = snoop(*s, k, h, &sp);
            if (v) return fail(err, "hart %d, line %08x: %s", h, t.snp_line, viol_name(v));
            if (s->line[h][a].st != t.snp_new || (sp == 1) != t.supply_arr || (sp == 2) != t.supply_wb)
                return fail(err, "hart %d: after the snoop of %08x the model has %s", h, t.snp_line,
                            st_str(s->line[h][a].st));
        }
        // 2. fills
        for (int h = 0; h < k.n; h++) {
            const T &t = tr[(size_t)h];
            if (!t.fill) continue;
            std::snprintf(x, sizeof x, ",\"cmd\":\"%s\",\"op\":\"%s\",\"to\":\"%s\",\"sc_ok\":%d", cmd_str(t.fill_cmd),
                          op_str(t.perf_kind), st_str(t.fill_st), (int)t.sc_ok);
            log(f, cyc, h, "fill", t.miss_line, x);
            if (!err) continue;
            checked++;
            int a = idx(t.miss_line, err);
            if (a < 0) return false;
            if (!s->busy || s->owner != h || s->ba != a || !can_complete(*s))
                return fail(err, "hart %d completed a fill of %08x the model cannot complete%s", h, t.miss_line);
            int ok = 0;
            int v = complete(*s, k, &ok);
            if (v) return fail(err, "hart %d, line %08x: %s", h, t.miss_line, viol_name(v));
            int want = (t.perf_kind == ST || t.perf_kind == AMO || (t.perf_kind == SC && t.sc_ok)) ? M : t.fill_st;
            if (s->line[h][a].st != want || (t.perf_kind == SC && ok != (int)t.sc_ok))
                return fail(err, "hart %d: after the fill of %08x the model has %s", h, t.miss_line,
                            st_str(s->line[h][a].st));
        }
        // 3. accesses that hit (or SCs that fail at once), and misses
        for (int h = 0; h < k.n; h++) {
            const T &t = tr[(size_t)h];
            if (t.perf && !t.fill) {
                std::snprintf(x, sizeof x, ",\"op\":\"%s\",\"from\":\"%s\",\"to\":\"%s\",\"sc_ok\":%d", op_str(t.perf_kind),
                              st_str(t.hit_old), st_str(t.hit_new), (int)t.sc_ok);
                log(f, cyc, h, "hit", t.perf_addr & ~15u, x);
                if (err) {
                    checked++;
                    int a = idx(t.perf_addr & ~15u, err);
                    if (a < 0) return false;
                    int kind = classify(*s, h, a, t.perf_kind);
                    if (s->pend[h] || kind == A_MISS || s->line[h][a].st != t.hit_old)
                        return fail(err, "hart %d performed an access to %08x without a miss; the model disagrees%s", h,
                                    t.perf_addr & ~15u);
                    int ok = 0;
                    int v = access(*s, k, h, a, t.perf_kind, -1, &ok);
                    if (v) return fail(err, "hart %d, line %08x: %s", h, t.perf_addr & ~15u, viol_name(v));
                    if (s->line[h][a].st != t.hit_new || (t.perf_kind == SC && ok != (int)t.sc_ok))
                        return fail(err, "hart %d: after the access to %08x the model has %s", h, t.perf_addr & ~15u,
                                    st_str(s->line[h][a].st));
                }
            }
            if (t.miss) {
                std::snprintf(x, sizeof x, ",\"op\":\"%s\",\"cmd\":\"%s\",\"victim\":\"%s\",\"vst\":\"%s\"", op_str(t.perf_kind),
                              cmd_str(t.miss_cmd), t.vic ? "" : "-", t.vic ? st_str(t.vic_st) : "-");
                log(f, cyc, h, "miss", t.miss_line, x);
                if (t.vic && f) {
                    std::snprintf(x, sizeof x, ",\"from\":\"%s\",\"wb\":%d", st_str(t.vic_st), (int)t.vic_dirty);
                    log(f, cyc, h, "evict", t.vic_line, x);
                }
                if (err) {
                    checked++;
                    int a = idx(t.miss_line, err);
                    int va = t.vic ? idx(t.vic_line, err) : -1;
                    if (a < 0 || (t.vic && va < 0)) return false;
                    if (!can_access(*s, h, a, t.perf_kind) || classify(*s, h, a, t.perf_kind) != A_MISS)
                        return fail(err, "hart %d missed on %08x; the model does not allow a miss%s", h, t.miss_line);
                    if (t.vic && (va == a || s->line[h][va].st != t.vic_st))
                        return fail(err, "hart %d evicted %08x in a state the model does not have%s", h, t.vic_line);
                    int v = access(*s, k, h, a, t.perf_kind, va);
                    if (v) return fail(err, "hart %d, line %08x: %s", h, t.miss_line, viol_name(v));
                    if (s->pcmd[h] != t.miss_cmd)
                        return fail(err, "hart %d: for %08x the model would send %s", h, t.miss_line, cmd_str(s->pcmd[h]));
                }
            }
        }
        // 4. bus grants (instruction fetches and uncached accesses are not part of the protocol)
        if (bt.gnt && bt.owner < k.n && bt.cmd <= WB) {
            int h = bt.owner;
            std::snprintf(x, sizeof x, ",\"cmd\":\"%s\"", cmd_str(bt.cmd));
            log(f, cyc, h, "grant", bt.addr & ~15u, x);
            if (err) {
                checked++;
                int a = idx(bt.addr & ~15u, err);
                if (a < 0) return false;
                if (bt.cmd == WB) {
                    if (!can_grant_wb(*s, h) || s->wa[h] != a)
                        return fail(err, "hart %d wrote back %08x; the model has no such write-back%s", h, bt.addr);
                    grant_wb(*s, k, h);
                } else {
                    if (!can_grant(*s, h) || s->pa[h] != a || s->pcmd[h] != bt.cmd)
                        return fail(err, "hart %d was granted %08x; the model has a different request%s", h, bt.addr);
                    grant(*s, k, h);
                }
            }
        }
        return true;
    }
};
#endif

}  // namespace mesi
