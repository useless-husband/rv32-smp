// Decoding of the per-cycle trace records the RTL exports for the harness:
// one 256-bit record per L1 data cache (rtl/l1d.sv, signal `trace`) and one
// 64-bit record for the bus (rtl/bus.sv).  The bit positions here and in the
// RTL must agree; tests/system/test_trace_layout.py checks the field list.
#pragma once
#include <cstdint>

enum { ST_I = 0, ST_S = 1, ST_E = 2, ST_M = 3 };
enum { CMD_RD = 0, CMD_RDX = 1, CMD_UPGR = 2, CMD_WB = 3, CMD_IRD = 4, CMD_UNC = 5 };
enum { K_LD = 0, K_ST = 1, K_LR = 2, K_SC = 3, K_AMO = 4 };

static inline const char *st_name(int s) { static const char *n[4] = {"I", "S", "E", "M"}; return n[s & 3]; }
static inline const char *cmd_name(int c)
{
    static const char *n[8] = {"BusRd", "BusRdX", "BusUpgr", "BusWB", "IFetch", "Uncached", "?", "?"};
    return n[c & 7];
}
static inline const char *kind_name(int k)
{
    static const char *n[8] = {"load", "store", "lr", "sc", "amo", "?", "?", "?"};
    return n[k & 7];
}

struct L1Trace {
    bool perf, miss, vic, snp, fill, wb_done, vic_dirty, wb_hit, snp_resv_clr, snp_upg_conv, shared, sc_ok;
    int perf_kind, miss_cmd, snp_cmd, fill_cmd, hit_old, hit_new, snp_old, snp_new, vic_st, fill_st;
    bool supply_arr, supply_wb, defer, snp_pending;
    int lock_cnt;
    uint32_t perf_addr, perf_old, perf_new, wmask, miss_line, vic_line, snp_line;
};

static inline uint32_t bits(uint32_t w, int lo, int n) { return (w >> lo) & ((1u << n) - 1); }

// w: the eight 32-bit words of one record, least significant first
static inline L1Trace decode_l1(const uint32_t *w)
{
    L1Trace t;
    uint32_t f = w[0], g = w[7];
    t.perf = bits(f, 0, 1);
    t.miss = bits(f, 1, 1);
    t.vic = bits(f, 2, 1);
    t.snp = bits(f, 3, 1);
    t.fill = bits(f, 4, 1);
    t.wb_done = bits(f, 5, 1);
    t.vic_dirty = bits(f, 6, 1);
    t.wb_hit = bits(f, 7, 1);
    t.snp_resv_clr = bits(f, 8, 1);
    t.snp_upg_conv = bits(f, 9, 1);
    t.shared = bits(f, 10, 1);
    t.sc_ok = bits(f, 11, 1);
    t.perf_kind = bits(f, 12, 3);
    t.miss_cmd = bits(f, 15, 3);
    t.snp_cmd = bits(f, 18, 3);
    t.fill_cmd = bits(f, 21, 3);
    t.hit_old = bits(f, 24, 2);
    t.hit_new = bits(f, 26, 2);
    t.snp_old = bits(f, 28, 2);
    t.snp_new = bits(f, 30, 2);
    t.perf_addr = w[1];
    t.perf_old = w[2];
    t.perf_new = w[3];
    t.miss_line = w[4];
    t.vic_line = w[5];
    t.snp_line = w[6];
    t.vic_st = bits(g, 0, 2);
    t.fill_st = bits(g, 2, 2);
    t.wmask = bits(g, 4, 4);
    t.supply_arr = bits(g, 8, 1);
    t.supply_wb = bits(g, 9, 1);
    t.defer = bits(g, 10, 1);
    t.snp_pending = bits(g, 11, 1);
    t.lock_cnt = bits(g, 12, 6);
    return t;
}

struct BusTrace {
    bool resp, supplied, shared, gnt;
    int owner, cmd;
    uint32_t addr;
};

static inline BusTrace decode_bus(uint64_t v)
{
    BusTrace b;
    b.addr = (uint32_t)v;
    b.cmd = (int)((v >> 53) & 7);
    b.owner = (int)((v >> 56) & 15);
    b.gnt = (v >> 60) & 1;
    b.shared = (v >> 61) & 1;
    b.supplied = (v >> 62) & 1;
    b.resp = (v >> 63) & 1;
    return b;
}
