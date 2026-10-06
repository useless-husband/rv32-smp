// Verilator harness for the multicore (rtl/sim/smp_top.sv).
//
// Each cycle it
//   1. reads every L1's trace record and feeds the accesses performed in
//      that cycle to the golden memory checker (sim/memcheck.h), which keeps
//      memory in perform order and checks every value read;
//   2. optionally steps the protocol model (model/mesi.hpp) with every
//      coherence event (miss, bus grant, snoop answer, fill, write-back) and
//      checks that the model allows it and reaches the same line states;
//   3. checks every committed instruction of every hart against that hart's
//      own golden model instance (lockstep).  A load's value depends on how
//      the harts interleave, so the model takes it from the access the L1
//      performed - already checked in step 1 - and checks everything else.
//
// Timing noise for the litmus and stress tests: --stall-pct injects fetch
// bubbles per hart, --jitter adds random latency to memory requests.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vtop.h"
#include "verilated.h"

extern "C" {
#include "rv_iss.h"
}
#include "memcheck.h"
#include "mesi.hpp"
#include "trace.h"

static const char *kEventNames[] = RV_HPM_NAMES;

double sc_time_stamp() { return 0; }

// ----------------------------------------------------- bit access helpers
static uint32_t gb(uint64_t v, int lo, int n)
{
    return (uint32_t)((v >> lo) & (n >= 32 ? 0xffffffffull : ((1ull << n) - 1)));
}
template <std::size_t W> static uint32_t gb(const VlWide<W> &v, int lo, int n)
{
    uint64_t x = v[(size_t)lo / 32];
    if ((size_t)lo / 32 + 1 < W) x |= (uint64_t)v[(size_t)lo / 32 + 1] << 32;
    return gb(x, lo % 32, n);
}

struct Options {
    std::string elf, json, coh_log, trace;
    uint64_t max_cycles = 200000000ull;
    bool lockstep = true, stats = false, quiet = false, model = false;
    int run_hart = -1;           // -1: all harts run
    uint32_t seed = 1;
    int stall_pct = 0, jitter = 0;
    int model_bug = -1;          // protocol variant the model follows (-1: the one the RTL was built with)
};

static void usage()
{
    std::fprintf(stderr,
                 "usage: vsmp [--max-cycles N] [--no-lockstep] [--run-hart K] [--seed S] [--stall-pct P]\n"
                 "            [--jitter J] [--model] [--coh-log FILE] [--trace FILE] [--json FILE] [--stats]\n"
                 "            [--quiet] prog.elf\n");
    std::exit(5);
}

static Options parse(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (a == "--max-cycles") o.max_cycles = std::strtoull(next(), nullptr, 0);
        else if (a == "--no-lockstep") o.lockstep = false;
        else if (a == "--run-hart") o.run_hart = std::atoi(next());
        else if (a == "--seed") o.seed = (uint32_t)std::strtoul(next(), nullptr, 0);
        else if (a == "--stall-pct") o.stall_pct = std::atoi(next());
        else if (a == "--jitter") o.jitter = std::atoi(next());
        else if (a == "--model") o.model = true;
        else if (a == "--coh-log") o.coh_log = next();
        else if (a == "--trace") o.trace = next();
        else if (a == "--json") o.json = next();
        else if (a == "--stats") o.stats = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a[0] != '-' && o.elf.empty()) o.elf = a;
        else usage();
    }
    if (o.elf.empty()) usage();
    return o;
}

// ------------------------------------------------- lockstep data hook
struct Harness {
    std::vector<std::deque<L1Trace>> q;   // performed, not yet committed, per hart
    std::string error;
};

static uint32_t data_hook(void *ctx, uint32_t hart, int kind, uint32_t addr, int size, uint32_t wv, int *sc_ok)
{
    Harness *H = (Harness *)ctx;
    (void)wv;
    if (!H->error.empty()) return 0;
    if (H->q[hart].empty()) {
        char b[160];
        std::snprintf(b, sizeof b, "hart %u: the model executes a %s at %08x the L1 never performed", hart,
                      kind_name(kind), addr);
        H->error = b;
        return 0;
    }
    L1Trace e = H->q[hart].front();
    H->q[hart].pop_front();
    if (e.perf_kind != kind || (e.perf_addr & ~3u) != (addr & ~3u)) {
        char b[200];
        std::snprintf(b, sizeof b, "hart %u: the model executes a %s at %08x, the L1 performed a %s at %08x", hart,
                      kind_name(kind), addr, kind_name(e.perf_kind), e.perf_addr);
        H->error = b;
        return 0;
    }
    if (sc_ok) *sc_ok = e.sc_ok;
    uint32_t sh = 8 * (addr & 3);
    uint32_t v = e.perf_old >> sh;
    return size == 1 ? (v & 0xff) : size == 2 ? (v & 0xffff) : v;
}

int main(int argc, char **argv)
{
    Options opt = parse(argc, argv);

    rv_iss iss0;
    if (rv_iss_init(&iss0) || rv_iss_load_elf(&iss0, opt.elf.c_str())) {
        std::fprintf(stderr, "vsmp: %s\n", iss0.errmsg);
        return 5;
    }
    std::string hex = opt.elf + "." + std::to_string(getpid()) + ".hex";
    if (rv_iss_write_hex(&iss0, hex.c_str())) {
        std::fprintf(stderr, "vsmp: cannot write %s\n", hex.c_str());
        return 5;
    }
    std::string plus = "+program=" + hex;
    const char *vargs[] = {argv[0], plus.c_str()};

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(2, vargs);
    auto top = std::make_unique<Vtop>(ctx.get());
    top->eval();
    const int N = (int)top->cfg_nharts;
    const int bug = (int)top->cfg_bug;
    if (opt.run_hart >= N) {
        std::fprintf(stderr, "vsmp: --run-hart %d but the build has %d harts\n", opt.run_hart, N);
        return 5;
    }

    // golden models: one per hart, sharing hart 0's RAM image
    Harness H;
    H.q.resize((size_t)N);
    std::vector<rv_iss> iss((size_t)N);
    iss0.nharts = (uint32_t)N;
    MemCheck mc;
    mc.init(iss0.ram, RV_RAM_SIZE, N);
    for (int h = 0; h < N; h++) {
        if (h == 0) iss[0] = iss0;
        else rv_iss_init_shared(&iss[(size_t)h], &iss[0], (uint32_t)h);
        iss[(size_t)h].nharts = (uint32_t)N;
        iss[(size_t)h].data_hook = data_hook;
        iss[(size_t)h].hook_ctx = &H;
    }
    for (int h = 1; h < N; h++) iss[(size_t)h].ram = iss[0].ram;

    // the protocol model, stepped with the RTL's coherence events
    mesi::Agreement agree(N, opt.model_bug >= 0 ? opt.model_bug : bug);
    FILE *coh = opt.coh_log.empty() ? nullptr : std::fopen(opt.coh_log.c_str(), "w");
    FILE *trace = opt.trace.empty() ? nullptr : std::fopen(opt.trace.c_str(), "w");

    std::mt19937 rng(opt.seed);
    uint32_t run_mask = opt.run_hart < 0 ? (1u << N) - 1 : 1u << opt.run_hart;

    top->clk = 0;
    top->rst = 1;
    top->hart_run = 0;
    top->hart_stall = 0;
    top->mem_jitter = 0;
    for (int i = 0; i < 4; i++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
    }
    std::remove(hex.c_str());
    top->clk = 0;
    top->rst = 0;
    top->hart_run = run_mask;
    top->eval();

    uint64_t cycle = 0, commits = 0;
    std::vector<uint64_t> retired((size_t)N, 0);
    std::vector<uint64_t> events((size_t)(N * RV_HPM_COUNT), 0);
    uint64_t bus_cmds[8] = {0}, bus_busy = 0, bus_supplied = 0;
    std::string console;
    bool exited = false;
    uint32_t exit_value = 0;
    uint64_t exit_cycle = 0;
    int status = -1;            // harness verdict: 2 lockstep, 3 timeout, 4 model error, 6 memory, 7 protocol model
    int prog_code = -1;         // the program's exit code
    bool busy = false;
    std::vector<std::vector<std::string>> recent((size_t)N);
    char line[320];
    std::vector<L1Trace> tr((size_t)N);
    uint32_t w[8];

    while (status < 0 && prog_code < 0) {
        // timing noise for the next edge
        uint32_t stall = 0;
        if (opt.stall_pct > 0)
            for (int h = 0; h < N; h++)
                if ((int)(rng() % 100) < opt.stall_pct) stall |= 1u << h;
        top->hart_stall = stall;
        top->mem_jitter = opt.jitter > 0 ? (uint8_t)(rng() % (uint32_t)(opt.jitter + 1)) : 0;
        top->clk = 0;
        top->eval();

        // 1. accesses performed this cycle -> memory checker, lockstep queues
        for (int h = 0; h < N; h++) {
            for (int k = 0; k < 8; k++) w[k] = gb(top->l1_trace, 256 * h + 32 * k, 32);
            tr[(size_t)h] = decode_l1(w);
        }
        BusTrace bt = decode_bus(top->bus_trace);
        if (!mc.cycle(cycle, tr)) {
            std::fprintf(stderr, "\nMEMORY CHECK FAILED: %s\n", mc.error.c_str());
            status = 6;
            break;
        }
        for (int h = 0; h < N; h++)
            if (tr[(size_t)h].perf) H.q[(size_t)h].push_back(tr[(size_t)h]);

        // 2. coherence events -> protocol model, log
        if (opt.model || coh) {
            std::string err;
            if (!agree.cycle(cycle, tr, bt, coh, opt.model ? &err : nullptr)) {
                std::fprintf(stderr, "\nMODEL DISAGREES at cycle %" PRIu64 ": %s\n", cycle, err.c_str());
                status = 7;
                break;
            }
        }
        if (bt.gnt) { bus_cmds[bt.cmd & 7]++; busy = true; }
        if (busy) bus_busy++;
        if (bt.resp) busy = false;
        if (bt.resp && bt.supplied) bus_supplied++;

        // 3. commits -> lockstep
        for (int h = 0; h < N && status < 0; h++) {
            if (!gb(top->commit_valid, h, 1)) continue;
            rv_commit d;
            std::memset(&d, 0, sizeof d);
            d.pc = gb(top->commit_pc, 32 * h, 32);
            d.insn = gb(top->commit_insn, 32 * h, 32);
            d.trap = (int)gb(top->commit_trap, h, 1);
            d.cause = d.trap ? gb(top->commit_cause, 32 * h, 32) : 0;
            d.rd_we = (int)gb(top->commit_rd_we, h, 1);
            d.rd = d.rd_we ? gb(top->commit_rd, 5 * h, 5) : 0;
            d.rd_val = d.rd_we ? gb(top->commit_rd_val, 32 * h, 32) : 0;
            d.mem_we = (int)gb(top->commit_mem_we, h, 1);
            d.mem_addr = d.mem_we ? gb(top->commit_mem_addr, 32 * h, 32) : 0;
            d.mem_wmask = d.mem_we ? gb(top->commit_mem_wmask, 4 * h, 4) : 0;
            uint32_t lanes = 0;
            for (int b = 0; b < 4; b++)
                if ((d.mem_wmask >> b) & 1) lanes |= 0xffu << (8 * b);
            d.mem_wdata = d.mem_we ? (gb(top->commit_mem_wdata, 32 * h, 32) & lanes) : 0;
            commits++;
            if (!d.trap) retired[(size_t)h]++;
            rv_format_commit(&d, line, sizeof line);
            if (trace) std::fprintf(trace, "%" PRIu64 " h%d %s\n", cycle, h, line);
            if (!opt.lockstep) continue;
            rv_iss &m = iss[(size_t)h];
            rv_commit mc_;
            m.have_override = 1;
            m.override_val = d.rd_val;
            rv_step(&m, &mc_);
            if (mc_.nondet && mc_.rd_we) mc_.rd_val = d.rd_val;
            if (m.error) {
                std::fprintf(stderr, "vsmp: golden model error (hart %d): %s\n", h, m.errmsg);
                status = 4;
                break;
            }
            bool same = d.pc == mc_.pc && d.insn == mc_.insn && d.trap == mc_.trap && d.cause == mc_.cause &&
                        d.rd_we == mc_.rd_we && d.rd == mc_.rd && d.rd_val == mc_.rd_val &&
                        d.mem_we == mc_.mem_we && d.mem_addr == mc_.mem_addr && d.mem_wdata == mc_.mem_wdata &&
                        d.mem_wmask == mc_.mem_wmask;
            if (!H.error.empty() || !same) {
                char ml[320];
                rv_format_commit(&mc_, ml, sizeof ml);
                std::fprintf(stderr, "\nLOCKSTEP MISMATCH on hart %d at commit %" PRIu64 ", cycle %" PRIu64 "\n", h,
                             commits, cycle);
                if (!H.error.empty()) std::fprintf(stderr, "  %s\n", H.error.c_str());
                std::fprintf(stderr, "  last matching commits of hart %d:\n", h);
                for (auto &r : recent[(size_t)h]) std::fprintf(stderr, "    %s\n", r.c_str());
                std::fprintf(stderr, "  core : %s\n  model: %s\n", line, ml);
                status = 2;
                break;
            }
            recent[(size_t)h].push_back(line);
            if (recent[(size_t)h].size() > 8) recent[(size_t)h].erase(recent[(size_t)h].begin());
        }
        if (status >= 0) break;

        for (int h = 0; h < N; h++)
            for (int e = 0; e < RV_HPM_COUNT; e++)
                if (gb(top->perf_events, RV_HPM_COUNT * h + e, 1)) events[(size_t)(h * RV_HPM_COUNT + e)]++;
        if (top->mmio_we) {
            if (top->mmio_addr == RV_MMIO_CONSOLE) {
                char ch = (char)(top->mmio_wdata & 0xff);
                console.push_back(ch);
                if (!opt.quiet) { std::putchar(ch); if (ch == '\n') std::fflush(stdout); }
            } else if (top->mmio_addr == RV_MMIO_EXIT && !exited) {
                exited = true;
                exit_value = top->mmio_wdata;
                exit_cycle = cycle;
            }
        }

        top->clk = 1;
        top->eval();
        cycle++;

        if (exited) {
            bool model_exit = false;
            for (int h = 0; h < N; h++) model_exit = model_exit || iss[(size_t)h].exited;
            if (!opt.lockstep || model_exit || cycle - exit_cycle > 64) {
                if (opt.lockstep && !model_exit) {
                    std::fprintf(stderr, "vsmp: the core wrote EXIT but no model did\n");
                    status = 2;
                } else {
                    prog_code = rv_exit_code(exit_value);
                }
            }
        } else if (cycle >= opt.max_cycles) {
            std::fprintf(stderr, "vsmp: no exit after %" PRIu64 " cycles\n", cycle);
            status = 3;
        }
    }
    std::fflush(stdout);
    if (coh) std::fclose(coh);
    if (trace) std::fclose(trace);
    top->final();

    uint64_t ret_all = 0;
    for (int h = 0; h < N; h++) ret_all += retired[(size_t)h];
    if (opt.stats) {
        std::fprintf(stderr, "cycles %" PRIu64 "  harts %d  instret", cycle, N);
        for (int h = 0; h < N; h++) std::fprintf(stderr, " %" PRIu64, retired[(size_t)h]);
        std::fprintf(stderr, "\n  bus: BusRd %" PRIu64 "  BusRdX %" PRIu64 "  BusUpgr %" PRIu64 "  BusWB %" PRIu64
                             "  IFetch %" PRIu64 "  Uncached %" PRIu64 "  supplied-by-cache %" PRIu64 "\n",
                     bus_cmds[0], bus_cmds[1], bus_cmds[2], bus_cmds[3], bus_cmds[4], bus_cmds[5], bus_supplied);
        std::fprintf(stderr, "  memory check: %" PRIu64 " accesses, %" PRIu64 " writes, SC %" PRIu64 " ok / %" PRIu64
                             " failed\n",
                     mc.checked, mc.writes, mc.sc_ok, mc.sc_fail);
        if (opt.model) std::fprintf(stderr, "  model agreement: %" PRIu64 " coherence events checked\n", agree.checked);
    }
    if (!opt.json.empty()) {
        FILE *f = std::fopen(opt.json.c_str(), "w");
        if (f) {
            std::fprintf(f, "{\"cycles\": %" PRIu64 ", \"harts\": %d, \"instret\": %" PRIu64 ", \"exit\": %d", cycle, N,
                         ret_all, status >= 0 ? status : prog_code);
            std::fprintf(f, ", \"bus\": {\"rd\": %" PRIu64 ", \"rdx\": %" PRIu64 ", \"upgr\": %" PRIu64 ", \"wb\": %" PRIu64
                            ", \"ifetch\": %" PRIu64 ", \"uncached\": %" PRIu64 ", \"supplied\": %" PRIu64
                            ", \"busy_cycles\": %" PRIu64 "}",
                         bus_cmds[0], bus_cmds[1], bus_cmds[2], bus_cmds[3], bus_cmds[4], bus_cmds[5], bus_supplied,
                         bus_busy);
            std::fprintf(f, ", \"memcheck\": {\"accesses\": %" PRIu64 ", \"writes\": %" PRIu64 ", \"sc_ok\": %" PRIu64
                            ", \"sc_fail\": %" PRIu64 "}, \"model_events\": %" PRIu64,
                         mc.checked, mc.writes, mc.sc_ok, mc.sc_fail, agree.checked);
            std::fprintf(f, ", \"per_hart\": [");
            for (int h = 0; h < N; h++) {
                std::fprintf(f, "%s{\"instret\": %" PRIu64, h ? ", " : "", retired[(size_t)h]);
                for (int e = 0; e < RV_HPM_COUNT; e++)
                    std::fprintf(f, ", \"%s\": %" PRIu64, kEventNames[e], events[(size_t)(h * RV_HPM_COUNT + e)]);
                std::fprintf(f, "}");
            }
            std::fprintf(f, "]}\n");
            std::fclose(f);
        }
    }
    for (int h = N - 1; h >= 0; h--) rv_iss_free(&iss[(size_t)h]);
    if (status >= 0) return status;
    if (prog_code != 0) {
        std::fprintf(stderr, "vsmp: program exit code %d\n", prog_code);
        return 1;
    }
    return 0;
}
