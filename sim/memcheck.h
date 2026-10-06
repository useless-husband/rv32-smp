// Golden memory checker.
//
// Every load, store, LR, SC and AMO is "performed" by an L1 in one cycle (at
// a hit, or in the cycle its fill arrives), and the L1 reports it (address,
// old word, new word).  The checker keeps its own copy of memory and applies
// the performs in cycle order.  A coherent memory system that performs every
// access atomically must then satisfy, for every access:
//
//   * the old word the L1 saw equals the checker's word: every read returns
//     the value of the latest write in perform order (this order is a legal
//     total order of all accesses, so every load value is explained by it);
//   * no two caches perform conflicting accesses (one of them a write) to one
//     line in the same cycle (that would mean two writable copies);
//   * an SC succeeds only if no other hart wrote its reservation granule
//     (the 16-byte line) since the hart's LR.
//
// The perform order is the order the hardware gives (each L1 blocks until an
// access is done, the pipeline issues them in program order), so it is also
// a witness that the hardware is sequentially consistent - stronger than the
// RVWMO model requires; docs/DESIGN.md, section 6.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "trace.h"

struct MemCheck {
    std::vector<uint8_t> mem;           // shadow of RAM
    uint32_t base = 0x80000000u, size = 0;
    struct Writer { int hart = -1; uint64_t cycle = 0; };
    std::vector<Writer> last;           // last writer of each word
    struct Resv { bool valid = false; uint32_t line = 0; };
    std::vector<Resv> ghost;            // per hart: LR done, no remote write since
    uint64_t checked = 0, writes = 0, sc_ok = 0, sc_fail = 0;
    std::string error;

    void init(const uint8_t *image, uint32_t bytes, int harts)
    {
        size = bytes;
        mem.assign(image, image + bytes);
        last.assign(bytes / 4, Writer());
        ghost.assign((size_t)harts, Resv());
    }

    uint32_t word(uint32_t a) const
    {
        uint32_t o = (a - base) & (size - 1) & ~3u;
        return mem[o] | mem[o + 1] << 8 | mem[o + 2] << 16 | (uint32_t)mem[o + 3] << 24;
    }

    // one cycle's performs, all harts; returns false on the first violation
    bool cycle(uint64_t cyc, const std::vector<L1Trace> &t)
    {
        // same-cycle conflicts
        for (size_t i = 0; i < t.size(); i++)
            for (size_t j = i + 1; j < t.size(); j++)
                if (t[i].perf && t[j].perf && (t[i].perf_addr >> 4) == (t[j].perf_addr >> 4) &&
                    writes_mem(t[i]) && writes_mem(t[j])) {
                    char b[200];
                    std::snprintf(b, sizeof b,
                                  "cycle %llu: harts %zu and %zu access line %08x in the same cycle, one of "
                                  "them writing (two writable copies)",
                                  (unsigned long long)cyc, i, j, t[i].perf_addr & ~15u);
                    error = b;
                    return false;
                }
        // apply reads before writes in the same cycle: a read and a write to one
        // line in one cycle linearise as read-then-write (the atomic bus never
        // grants two writers the same line at once, checked just above)
        for (size_t h = 0; h < t.size(); h++)
            if (t[h].perf && !writes_mem(t[h]) && !perform(cyc, (int)h, t[h]))
                return false;
        for (size_t h = 0; h < t.size(); h++)
            if (t[h].perf && writes_mem(t[h]) && !perform(cyc, (int)h, t[h]))
                return false;
        return true;
    }

    static bool writes_mem(const L1Trace &e)
    {
        return e.perf_kind == K_ST || e.perf_kind == K_AMO || (e.perf_kind == K_SC && e.sc_ok);
    }

    bool perform(uint64_t cyc, int h, const L1Trace &e)
    {
        uint32_t a = e.perf_addr & ~3u, want = word(a);
        checked++;
        // (a failed SC reads nothing: its old word is not architecturally visible)
        if (!(e.perf_kind == K_SC && !e.sc_ok) && e.perf_old != want) {
            const Writer &w = last[((a - base) & (size - 1)) / 4];
            char b[320];
            std::snprintf(b, sizeof b,
                          "cycle %llu: hart %d %s at %08x saw %08x, but the latest write in perform order "
                          "left %08x (%s%d at cycle %llu)",
                          (unsigned long long)cyc, h, kind_name(e.perf_kind), a, e.perf_old, want,
                          w.hart < 0 ? "program image" : "hart ", w.hart < 0 ? 0 : w.hart,
                          (unsigned long long)w.cycle);
            error = b;
            return false;
        }
        uint32_t line = a >> 4;
        if (e.perf_kind == K_LR) {
            ghost[(size_t)h].valid = true;
            ghost[(size_t)h].line = line;
        }
        if (e.perf_kind == K_SC) {
            bool g = ghost[(size_t)h].valid && ghost[(size_t)h].line == line;
            if (e.sc_ok && !g) {
                char b[200];
                std::snprintf(b, sizeof b,
                              "cycle %llu: hart %d SC at %08x succeeded although another hart wrote the "
                              "line after this hart's LR (atomicity broken)",
                              (unsigned long long)cyc, h, a);
                error = b;
                return false;
            }
            ghost[(size_t)h].valid = false;
            if (e.sc_ok) sc_ok++;
            else sc_fail++;
        }
        if (writes_mem(e)) {
            uint32_t o = (a - base) & (size - 1);
            for (int i = 0; i < 4; i++) mem[o + (uint32_t)i] = (uint8_t)(e.perf_new >> (8 * i));
            last[o / 4] = Writer{h, cyc};
            writes++;
            for (size_t g = 0; g < ghost.size(); g++)
                if ((int)g != h && ghost[g].valid && ghost[g].line == line) ghost[g].valid = false;
        }
        return true;
    }
};
