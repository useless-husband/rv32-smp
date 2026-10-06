// Explicit-state model checker for the protocol model (model/mesi.hpp).
//
// Breadth-first search over every reachable state of N caches and A line
// addresses, with symmetry reduction (caches are interchangeable, and so are
// addresses: a state is stored as the smallest encoding over all
// renamings).  Every action is checked against the invariants of mesi.hpp,
// and every reachable state against progress (the protocol alone can finish
// every outstanding request).  Breadth-first order makes the first
// counterexample found a shortest one; it is replayed without symmetry
// reduction so the trace names caches and addresses consistently.
//
//   mc --all                 the correct protocol, 2-3 caches x 1-2 addresses,
//                            and every bug variant (markdown table on stdout)
//   mc --bug K --n N --a A [--observable] [--json FILE]
//
// --observable ignores the internal invariants (single writer, data value)
// and looks for an effect a program can see: a load returning a stale value,
// an SC succeeding after a remote write, or a request that can never finish.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mesi.hpp"

using namespace mesi;
typedef State<2> St;
typedef unsigned __int128 Key;

// ------------------------------------------------------------- encoding
struct Codec {
    Cfg k;
    std::vector<std::vector<int>> cperm, aperm;

    explicit Codec(const Cfg &c) : k(c)
    {
        std::vector<int> p;
        for (int i = 0; i < k.n; i++) p.push_back(i);
        do cperm.push_back(p); while (std::next_permutation(p.begin(), p.end()));
        std::vector<int> q;
        for (int i = 0; i < k.a; i++) q.push_back(i);
        do aperm.push_back(q); while (std::next_permutation(q.begin(), q.end()));
    }

    void normalise(St &s) const
    {
        for (int c = 0; c < k.n; c++) {
            for (int a = 0; a < k.a; a++)
                if (s.line[c][a].st == I && k.bug != LOST_UPGRADE) s.line[c][a].fresh = 0;
            if (!s.pend[c]) s.pa[c] = s.pop[c] = s.pcmd[c] = 0;
            if (!s.wb[c]) s.wa[c] = s.wf[c] = 0;
            if (!s.resv[c]) s.ra[c] = s.intact[c] = s.young[c] = 0;
        }
        if (!s.busy) s.owner = s.bcmd = s.ba = s.need = s.sup = s.shared = 0;
        if (!s.sup) s.supf = 0;
    }

    St rename(const St &s, const std::vector<int> &p, const std::vector<int> &q) const
    {
        St t = s;
        for (int c = 0; c < k.n; c++) {
            int pc = p[(size_t)c];
            for (int a = 0; a < k.a; a++) t.line[pc][q[(size_t)a]] = s.line[c][a];
            t.pend[pc] = s.pend[c]; t.pa[pc] = (uint16_t)q[s.pa[c]]; t.pop[pc] = s.pop[c]; t.pcmd[pc] = s.pcmd[c];
            t.wb[pc] = s.wb[c]; t.wa[pc] = (uint16_t)q[s.wa[c]]; t.wf[pc] = s.wf[c];
            t.resv[pc] = s.resv[c]; t.ra[pc] = (uint16_t)q[s.ra[c]]; t.intact[pc] = s.intact[c]; t.young[pc] = s.young[c];
        }
        for (int a = 0; a < k.a; a++) t.memf[q[(size_t)a]] = s.memf[a];
        t.owner = (uint8_t)p[s.owner];
        t.ba = (uint16_t)q[s.ba];
        t.need = 0;
        for (int c = 0; c < k.n; c++)
            if ((s.need >> c) & 1) t.need |= (uint8_t)(1u << p[(size_t)c]);
        return t;
    }

    static void put(Key &x, int &pos, unsigned v, int w) { x |= (Key)v << pos; pos += w; }
    static unsigned get(Key x, int &pos, int w) { unsigned v = (unsigned)(x >> pos) & ((1u << w) - 1); pos += w; return v; }

    Key raw(const St &s) const
    {
        Key x = 0;
        int p = 0;
        for (int c = 0; c < k.n; c++) {
            for (int a = 0; a < k.a; a++) { put(x, p, s.line[c][a].st, 2); put(x, p, s.line[c][a].fresh, 1); }
            put(x, p, s.pend[c], 1); put(x, p, s.pa[c], 1); put(x, p, s.pop[c], 3); put(x, p, s.pcmd[c], 2);
            put(x, p, s.wb[c], 1); put(x, p, s.wa[c], 1); put(x, p, s.wf[c], 1);
            put(x, p, s.resv[c], 1); put(x, p, s.ra[c], 1); put(x, p, s.intact[c], 1); put(x, p, s.young[c], 1);
        }
        for (int a = 0; a < k.a; a++) put(x, p, s.memf[a], 1);
        put(x, p, s.busy, 1); put(x, p, s.owner, 2); put(x, p, s.bcmd, 2); put(x, p, s.ba, 1); put(x, p, s.need, 3);
        put(x, p, s.sup, 1); put(x, p, s.supf, 1); put(x, p, s.shared, 1);
        return x;
    }

    St decode(Key x) const
    {
        St s;
        int p = 0;
        for (int c = 0; c < k.n; c++) {
            for (int a = 0; a < k.a; a++) {
                s.line[c][a].st = (uint8_t)get(x, p, 2);
                s.line[c][a].fresh = (uint8_t)get(x, p, 1);
            }
            s.pend[c] = (uint8_t)get(x, p, 1); s.pa[c] = (uint16_t)get(x, p, 1); s.pop[c] = (uint8_t)get(x, p, 3);
            s.pcmd[c] = (uint8_t)get(x, p, 2);
            s.wb[c] = (uint8_t)get(x, p, 1); s.wa[c] = (uint16_t)get(x, p, 1); s.wf[c] = (uint8_t)get(x, p, 1);
            s.resv[c] = (uint8_t)get(x, p, 1); s.ra[c] = (uint16_t)get(x, p, 1); s.intact[c] = (uint8_t)get(x, p, 1);
            s.young[c] = (uint8_t)get(x, p, 1);
        }
        for (int a = 0; a < k.a; a++) s.memf[a] = (uint8_t)get(x, p, 1);
        s.busy = (uint8_t)get(x, p, 1); s.owner = (uint8_t)get(x, p, 2); s.bcmd = (uint8_t)get(x, p, 2);
        s.ba = (uint16_t)get(x, p, 1); s.need = (uint8_t)get(x, p, 3);
        s.sup = (uint8_t)get(x, p, 1); s.supf = (uint8_t)get(x, p, 1); s.shared = (uint8_t)get(x, p, 1);
        return s;
    }

    Key canon(St s) const
    {
        normalise(s);
        Key best = ~(Key)0;
        for (auto &p : cperm)
            for (auto &q : aperm) {
                St t = rename(s, p, q);
                normalise(t);   // renaming moves the "don't care" fields too
                Key x = raw(t);
                if (x < best) best = x;
            }
        return best;
    }
};

// ---------------------------------------------------------- transitions
struct Act {
    int type, c, a, op, victim;   // type: 0 access, 1 grant, 2 grant-wb, 3 snoop, 4 complete, 5 timeout
};

static std::string describe(const Act &x, const St &before, const St &after)
{
    char b[200];
    const char *an[2] = {"A", "B"};
    switch (x.type) {
    case 0: {
        int kind = classify(before, x.c, x.a, x.op);
        if (kind == A_SCFAIL) std::snprintf(b, sizeof b, "cache %d: SC to %s fails at once (no reservation)", x.c, an[x.a]);
        else if (kind == A_HIT) std::snprintf(b, sizeof b, "cache %d: %s %s hits (%s)", x.c, op_str(x.op), an[x.a],
                                              st_str(before.line[x.c][x.a].st));
        else {
            int n = std::snprintf(b, sizeof b, "cache %d: %s %s misses, waits for %s", x.c, op_str(x.op), an[x.a],
                                  cmd_str(after.pcmd[x.c]));
            if (x.victim >= 0)
                std::snprintf(b + n, sizeof b - (size_t)n, "; evicts %s (%s%s)", an[x.victim],
                              st_str(before.line[x.c][x.victim].st),
                              before.line[x.c][x.victim].st == M ? " -> write-back buffer" : "");
        }
        break;
    }
    case 1: std::snprintf(b, sizeof b, "bus: grants cache %d %s %s", x.c, cmd_str(before.pcmd[x.c]), an[before.pa[x.c]]); break;
    case 2: std::snprintf(b, sizeof b, "bus: cache %d writes back %s to memory", x.c, an[before.wa[x.c]]); break;
    case 3: std::snprintf(b, sizeof b, "cache %d: answers the snoop (%s -> %s)%s", x.c,
                          st_str(before.line[x.c][before.ba].st), st_str(after.line[x.c][before.ba].st),
                          (after.sup && !before.sup) ? ", supplies the line" : ""); break;
    case 4: std::snprintf(b, sizeof b, "cache %d: fill done, %s %s now %s, performs its %s", before.owner,
                          an[before.ba], "is", st_str(after.line[before.owner][before.ba].st),
                          op_str(before.pop[before.owner])); break;
    default: std::snprintf(b, sizeof b, "cache %d: LR lock-out window closes", x.c); break;
    }
    return b;
}

// apply x to s; returns a violation code
static int apply(St &s, const Cfg &k, const Act &x)
{
    switch (x.type) {
    case 0: return access(s, k, x.c, x.a, x.op, x.victim);
    case 1: grant(s, k, x.c); return OK;
    case 2: return grant_wb(s, k, x.c);
    case 3: return snoop(s, k, x.c);
    case 4: return complete(s, k);
    default: timeout(s, x.c); return OK;
    }
}

static void actions(const St &s, const Cfg &k, std::vector<Act> &out)
{
    out.clear();
    for (int c = 0; c < k.n; c++) {
        for (int a = 0; a < k.a; a++)
            for (int op = LD; op <= AMO; op++) {
                if (!can_access(s, c, a, op)) continue;
                out.push_back(Act{0, c, a, op, -1});
                if (classify(s, c, a, op) == A_MISS && s.line[c][a].st == I)
                    for (int b = 0; b < k.a; b++)
                        if (b != a && s.line[c][b].st != I) out.push_back(Act{0, c, a, op, b});
            }
        if (can_grant(s, c)) out.push_back(Act{1, c, 0, 0, -1});
        if (can_grant_wb(s, c)) out.push_back(Act{2, c, 0, 0, -1});
        if (can_snoop(s, c)) out.push_back(Act{3, c, 0, 0, -1});
        if (can_timeout(s, k, c)) out.push_back(Act{5, c, 0, 0, -1});
    }
    if (can_complete(s)) out.push_back(Act{4, 0, 0, 0, -1});
}

// ------------------------------------------------------------ the search
struct Table {   // open addressing: key -> state index
    std::vector<Key> keys;
    std::vector<uint32_t> slot;
    size_t mask = 0;
    Table() { slot.assign(1u << 16, 0); mask = slot.size() - 1; }
    static size_t h(Key x) { uint64_t a = (uint64_t)x, b = (uint64_t)(x >> 64); a ^= b * 0x9e3779b97f4a7c15ull; a ^= a >> 29; a *= 0xbf58476d1ce4e5b9ull; return (size_t)(a ^ (a >> 32)); }
    // returns index; *fresh = true if inserted
    uint32_t insert(Key x, bool *fresh)
    {
        if (keys.size() * 2 >= slot.size()) grow();
        size_t i = h(x) & mask;
        while (slot[i]) {
            if (keys[slot[i] - 1] == x) { *fresh = false; return slot[i] - 1; }
            i = (i + 1) & mask;
        }
        keys.push_back(x);
        slot[i] = (uint32_t)keys.size();
        *fresh = true;
        return (uint32_t)keys.size() - 1;
    }
    void grow()
    {
        slot.assign(slot.size() * 2, 0);
        mask = slot.size() - 1;
        for (size_t j = 0; j < keys.size(); j++) {
            size_t i = h(keys[j]) & mask;
            while (slot[i]) i = (i + 1) & mask;
            slot[i] = (uint32_t)j + 1;
        }
    }
};

struct Result {
    uint64_t states = 0, transitions = 0;
    int depth = 0;
    double seconds = 0;
    int viol = OK;              // or -1: progress
    std::vector<std::string> steps;
    std::vector<St> snaps;
};

static Result run(const Cfg &k, bool observable)
{
    auto t0 = std::chrono::steady_clock::now();
    Codec cd(k);
    Table T;
    std::vector<uint32_t> parent;
    std::vector<uint32_t> level;
    Result r;
    St init;
    bool fresh;
    T.insert(cd.canon(init), &fresh);
    parent.push_back(0);
    level.push_back(0);
    std::vector<Act> acts;
    int64_t bad_parent = -1, bad_state = -1;
    int bad_code = OK;
    for (size_t i = 0; i < T.keys.size() && bad_parent < 0; i++) {
        St s = cd.decode(T.keys[i]);
        if (!drains(s, k)) { bad_parent = (int64_t)i; bad_state = (int64_t)i; bad_code = -1; break; }
        actions(s, k, acts);
        for (auto &x : acts) {
            St t = s;
            int v = apply(t, k, x);
            if (observable && (v == V_SWMR || v == V_DATA)) v = OK;
            r.transitions++;
            if (v) { bad_parent = (int64_t)i; bad_code = v; bad_state = -2; break; }
            T.insert(cd.canon(t), &fresh);
            if (fresh) {
                parent.push_back((uint32_t)i);
                level.push_back(level[i] + 1);
                r.depth = std::max(r.depth, (int)level.back());
            }
        }
    }
    r.states = T.keys.size();
    r.viol = bad_code;
    if (bad_parent >= 0) {
        // the canonical path from the initial state to the bad state
        std::vector<Key> path;
        for (int64_t i = bad_parent;; i = parent[(size_t)i]) {
            path.push_back(T.keys[(size_t)i]);
            if (i == 0) break;
        }
        std::reverse(path.begin(), path.end());
        // replay concretely: pick an action whose result has the next canonical key
        St s;
        r.snaps.push_back(s);
        for (size_t p = 1; p < path.size(); p++) {
            actions(s, k, acts);
            bool found = false;
            for (auto &x : acts) {
                St t = s;
                Cfg q = k;
                apply(t, q, x);
                if (cd.canon(t) == path[p]) {
                    r.steps.push_back(describe(x, s, t));
                    s = t;
                    r.snaps.push_back(s);
                    found = true;
                    break;
                }
            }
            if (!found) { r.steps.push_back("(replay failed)"); break; }
        }
        if (bad_code == -1) {
            // show why: let the protocol run on (no core accesses) until it is stuck
            for (int guard = 0; guard < 32; guard++) {
                actions(s, k, acts);
                const Act *pick = nullptr;
                for (auto &x : acts)
                    if (x.type != 0 && x.type != 5) { pick = &x; break; }
                if (!pick) break;
                St t = s;
                apply(t, k, *pick);
                r.steps.push_back(describe(*pick, s, t));
                s = t;
                r.snaps.push_back(s);
            }
            char b[200];
            std::snprintf(b, sizeof b, "stuck: the bus waits for cache %d, which defers the snoop until its own core "
                                       "performs an SC - no protocol action is left", (int)__builtin_ctz(s.need ? s.need : 1));
            r.steps.push_back(b);
            r.snaps.push_back(s);
        }
        if (bad_state == -2) {   // the violating action itself
            actions(s, k, acts);
            for (auto &x : acts) {
                St t = s;
                int v = apply(t, k, x);
                if (observable && (v == V_SWMR || v == V_DATA)) v = OK;
                if (v == bad_code) {
                    r.steps.push_back(describe(x, s, t));
                    r.snaps.push_back(t);
                    break;
                }
            }
        }
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

static const char *bug_name(int b)
{
    static const char *n[] = {"correct protocol", "lost upgrade", "write-back buffer not snooped",
                              "LR reservation survives a remote write", "E despite a shared copy",
                              "LR lock-out without timeout"};
    return n[b];
}

static const char *verdict(int v) { return v == -1 ? "a request can never finish (no progress)" : viol_name(v); }

static void write_json(const char *path, const Cfg &k, const Result &r, bool observable)
{
    FILE *f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "{\"bug\": %d, \"name\": \"%s\", \"caches\": %d, \"addresses\": %d, \"observable\": %s,\n", k.bug,
                 bug_name(k.bug), k.n, k.a, observable ? "true" : "false");
    std::fprintf(f, " \"states\": %llu, \"violation\": \"%s\",\n \"steps\": [\n", (unsigned long long)r.states,
                 r.viol == OK ? "none" : verdict(r.viol));
    for (size_t i = 0; i < r.snaps.size(); i++) {
        const St &s = r.snaps[i];
        std::fprintf(f, "  {\"action\": \"%s\", \"caches\": [", i == 0 ? "initial state" : r.steps[i - 1].c_str());
        for (int c = 0; c < k.n; c++) {
            std::fprintf(f, "%s{\"lines\": [", c ? ", " : "");
            for (int a = 0; a < k.a; a++)
                std::fprintf(f, "%s{\"st\": \"%s\", \"fresh\": %d}", a ? ", " : "", st_str(s.line[c][a].st),
                             s.line[c][a].fresh);
            std::fprintf(f, "], \"pend\": \"%s\", \"wb\": \"%s\", \"resv\": \"%s\"}",
                         s.pend[c] ? (std::string(op_str(s.pop[c])) + " " + "AB"[s.pa[c]] + " via " + cmd_str(s.pcmd[c])).c_str() : "",
                         s.wb[c] ? (std::string(1, "AB"[s.wa[c]]) + (s.wf[c] ? " (current)" : " (stale)")).c_str() : "",
                         s.resv[c] ? (std::string(1, "AB"[s.ra[c]]) + (s.young[c] ? ", lock-out" : "")).c_str() : "");
        }
        std::fprintf(f, "], \"mem\": [");
        for (int a = 0; a < k.a; a++) std::fprintf(f, "%s%d", a ? ", " : "", s.memf[a]);
        std::fprintf(f, "], \"bus\": \"%s\"}%s\n",
                     s.busy ? (std::string("cache ") + char('0' + s.owner) + " " + cmd_str(s.bcmd) + " " + "AB"[s.ba]).c_str() : "",
                     i + 1 < r.snaps.size() ? "," : "");
    }
    std::fprintf(f, " ]}\n");
    std::fclose(f);
}

int main(int argc, char **argv)
{
    Cfg k;
    bool all = false, observable = false;
    const char *json = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { if (i + 1 >= argc) std::exit(5); return std::atoi(argv[++i]); };
        if (a == "--all") all = true;
        else if (a == "--bug") k.bug = next();
        else if (a == "--n") k.n = next();
        else if (a == "--a") k.a = next();
        else if (a == "--observable") observable = true;
        else if (a == "--json" && i + 1 < argc) json = argv[++i];
        else { std::fprintf(stderr, "usage: mc --all | --bug K --n N --a A [--observable] [--json FILE]\n"); return 5; }
    }
    if (!all) {
        Result r = run(k, observable);
        std::printf("%s, %d caches, %d addresses: %llu states, %llu transitions, depth %d, %.2f s: %s\n",
                    bug_name(k.bug), k.n, k.a, (unsigned long long)r.states, (unsigned long long)r.transitions,
                    r.depth, r.seconds, r.viol == OK ? "all properties hold" : verdict(r.viol));
        for (size_t i = 0; i < r.steps.size(); i++) std::printf("  %2zu. %s\n", i + 1, r.steps[i].c_str());
        if (json) write_json(json, k, r, observable);
        return r.viol == OK ? 0 : 1;
    }
    int bad = 0;
    std::printf("## Correct protocol: exhaustive search\n\n");
    std::printf("| caches | addresses | states (after symmetry reduction) | transitions | depth | time | result |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|---|\n");
    int cfgs[4][2] = {{2, 1}, {2, 2}, {3, 1}, {3, 2}};
    for (auto &c : cfgs) {
        k.bug = NONE; k.n = c[0]; k.a = c[1];
        Result r = run(k, false);
        std::printf("| %d | %d | %llu | %llu | %d | %.2f s | %s |\n", k.n, k.a, (unsigned long long)r.states,
                    (unsigned long long)r.transitions, r.depth, r.seconds,
                    r.viol == OK ? "all properties hold" : verdict(r.viol));
        std::fflush(stdout);
        if (r.viol != OK) bad = 1;
    }
    std::printf("\n## Bug variants: shortest counterexamples\n\n");
    std::printf("| variant | first internal invariant broken | steps | first visible effect | steps | caches x addresses |\n");
    std::printf("|---|---|---:|---|---:|---|\n");
    for (int b = 1; b <= 5; b++) {
        Result ri, ro;
        Cfg kk;
        for (auto &c : cfgs) {
            kk.bug = b; kk.n = c[0]; kk.a = c[1];
            ro = run(kk, true);
            if (ro.viol != OK) break;
        }
        ri = run(kk, false);
        std::printf("| %s | %s | %zu | %s | %zu | %d x %d |\n", bug_name(b), ri.viol == OK ? "none" : verdict(ri.viol),
                    ri.steps.size(), ro.viol == OK ? "none found" : verdict(ro.viol), ro.steps.size(), kk.n, kk.a);
        std::fflush(stdout);
        char path[64];
        std::snprintf(path, sizeof path, "build/museum/mc_bug%d.json", b);
        write_json(path, kk, ro, true);
        if (ro.viol == OK) bad = 1;
    }
    return bad;
}
