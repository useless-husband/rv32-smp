// Verilator harness shared by both cores (the model class is always Vtop).
//
// Runs a RISC-V ELF program on the simulated core and, unless --no-lockstep
// is given, steps the golden model (model/rv_iss.c) once for every record
// the core puts on its commit port and compares the two field by field.
// The first difference stops the run with a report.
//
//   vsim [options] program.elf
//     --max-cycles N     give up after N cycles (default 200M)
//     --no-lockstep      do not run the golden model
//     --trace FILE       write the commit trace
//     --json FILE        write cycle/instret/event counts as JSON
//     --pipeview FILE    record per-cycle pipeline occupancy (pipelined core)
//     --pv-from N        ... starting at cycle N (default 0)
//     --pv-cycles N      ... for N cycles (default 400)
//     --stats            print the statistics
//     --quiet            do not echo the program's console output
//
// Exit status: 0 = the program exited with code 0, 1 = it exited with
// another code (printed), 2 = lockstep mismatch, 3 = cycle limit,
// 4 = golden-model error, 5 = usage or load error.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vtop.h"
#include "verilated.h"

extern "C" {
#include "rv_iss.h"
}

static const char *kEventNames[] = RV_HPM_NAMES;

double sc_time_stamp() { return 0; } // needed by libverilated when not using SystemC

struct Options {
    std::string elf, trace, json, pipeview;
    uint64_t max_cycles = 200000000ull;
    uint64_t pv_from = 0, pv_cycles = 400;
    bool lockstep = true, stats = false, quiet = false;
};

static void usage()
{
    std::fprintf(stderr, "usage: vsim [--max-cycles N] [--no-lockstep] [--trace FILE] [--json FILE]\n"
                         "            [--pipeview FILE] [--pv-from N] [--pv-cycles N] [--stats] [--quiet] prog.elf\n");
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
        else if (a == "--trace") o.trace = next();
        else if (a == "--json") o.json = next();
        else if (a == "--pipeview") o.pipeview = next();
        else if (a == "--pv-from") o.pv_from = std::strtoull(next(), nullptr, 0);
        else if (a == "--pv-cycles") o.pv_cycles = std::strtoull(next(), nullptr, 0);
        else if (a == "--stats") o.stats = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a[0] != '-' && o.elf.empty()) o.elf = a;
        else usage();
    }
    if (o.elf.empty()) usage();
    return o;
}

static rv_commit dut_commit(const Vtop *t)
{
    rv_commit c;
    std::memset(&c, 0, sizeof c);
    c.pc = t->commit_pc;
    c.insn = t->commit_insn;
    c.trap = t->commit_trap;
    c.cause = c.trap ? t->commit_cause : 0;
    c.rd_we = t->commit_rd_we;
    c.rd = c.rd_we ? t->commit_rd : 0;
    c.rd_val = c.rd_we ? t->commit_rd_val : 0;
    c.mem_we = t->commit_mem_we;
    c.mem_addr = c.mem_we ? t->commit_mem_addr : 0;
    c.mem_wmask = c.mem_we ? t->commit_mem_wmask : 0;
    // only the written byte lanes are meaningful (the cores replicate SB/SH data)
    uint32_t lanes = 0;
    for (int b = 0; b < 4; b++)
        if ((c.mem_wmask >> b) & 1) lanes |= 0xffu << (8 * b);
    c.mem_wdata = c.mem_we ? (t->commit_mem_wdata & lanes) : 0;
    // F and D (constant zero on a core without the FPU)
    c.frd_we = t->commit_frd_we;
    c.frd = c.frd_we ? t->commit_rd : 0;
    c.frd_val = c.frd_we ? t->commit_frd_val : 0;
    c.fflags = c.trap ? 0 : t->commit_fflags;
    c.mem_dbl = c.mem_we && t->commit_mem_dbl;
    c.mem_wdata_hi = c.mem_dbl ? t->commit_mem_wdata_hi : 0;
    return c;
}

static bool same(const rv_commit &a, const rv_commit &b)
{
    return a.pc == b.pc && a.insn == b.insn && a.trap == b.trap && a.cause == b.cause && a.rd_we == b.rd_we &&
           a.rd == b.rd && a.rd_val == b.rd_val && a.mem_we == b.mem_we && a.mem_addr == b.mem_addr &&
           a.mem_wdata == b.mem_wdata && a.mem_wmask == b.mem_wmask && a.frd_we == b.frd_we &&
           a.frd == b.frd && a.frd_val == b.frd_val && a.fflags == b.fflags && a.mem_dbl == b.mem_dbl &&
           a.mem_wdata_hi == b.mem_wdata_hi;
}

#ifdef HAVE_PIPEVIEW
#include "pipeview.inc"
#endif

int main(int argc, char **argv)
{
    Options opt = parse(argc, argv);

    rv_iss iss;
    if (rv_iss_init(&iss) || rv_iss_load_elf(&iss, opt.elf.c_str())) {
        std::fprintf(stderr, "vsim: %s\n", iss.errmsg);
        return 5;
    }
    // The RTL memory is loaded with $readmemh from a private temporary file.
    std::string hex = opt.elf + "." + std::to_string(getpid()) + ".hex";
    if (rv_iss_write_hex(&iss, hex.c_str())) {
        std::fprintf(stderr, "vsim: cannot write %s\n", hex.c_str());
        return 5;
    }
    std::string plus = "+program=" + hex;
    const char *vargs[] = {argv[0], plus.c_str()};

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(2, vargs);
    auto top = std::make_unique<Vtop>(ctx.get());
    top->eval();
    iss.has_fpu = top->cfg_fpu; // the golden model implements what the core was built with

    FILE *trace = opt.trace.empty() ? nullptr : std::fopen(opt.trace.c_str(), "w");
#ifdef HAVE_PIPEVIEW
    PipeView pv(opt.pipeview, opt.pv_from, opt.pv_cycles);
#else
    if (!opt.pipeview.empty()) std::fprintf(stderr, "vsim: --pipeview needs the pipelined core\n");
#endif

    top->clk = 0;
    top->rst = 1;
    for (int i = 0; i < 4; i++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
    }
    std::remove(hex.c_str());
    top->clk = 0;
    top->rst = 0;
    top->eval();

    uint64_t cycle = 0, commits = 0, retired = 0;
    uint64_t events[RV_HPM_COUNT] = {0};
    std::string console;
    bool exited = false;
    uint32_t exit_value = 0;
    uint64_t exit_cycle = 0;
    int status = -1;
    std::vector<std::string> recent; // last few commits, printed on a mismatch
    char line[320];

    while (status < 0) {
        top->clk = 0;
        top->eval();

        if (top->commit_valid) {
            rv_commit d = dut_commit(top.get());
            commits++;
            if (!d.trap) retired++;
            rv_format_commit(&d, line, sizeof line);
            if (trace) std::fprintf(trace, "%s\n", line);
            if (opt.lockstep) {
                rv_commit m;
                iss.have_override = 1;
                iss.override_val = d.rd_val;
                rv_step(&iss, &m);
                if (m.nondet && m.rd_we) m.rd_val = d.rd_val; // counter reads are timing dependent
                if (iss.error) {
                    std::fprintf(stderr, "vsim: golden model error: %s\n", iss.errmsg);
                    status = 4;
                    break;
                }
                if (!same(d, m)) {
                    char ml[320];
                    rv_format_commit(&m, ml, sizeof ml);
                    std::fprintf(stderr, "\nLOCKSTEP MISMATCH at commit %" PRIu64 ", cycle %" PRIu64 "\n", commits, cycle);
                    std::fprintf(stderr, "  last matching commits:\n");
                    for (auto &r : recent) std::fprintf(stderr, "    %s\n", r.c_str());
                    std::fprintf(stderr, "  core : %s\n  model: %s\n", line, ml);
                    status = 2;
                    break;
                }
                recent.push_back(line);
                if (recent.size() > 8) recent.erase(recent.begin());
            }
        }
        for (int e = 0; e < RV_HPM_COUNT; e++)
            if ((top->perf_events >> e) & 1) events[e]++;
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
#ifdef HAVE_PIPEVIEW
        pv.sample(top.get(), cycle, &iss);
#endif

        top->clk = 1;
        top->eval();
        cycle++;

        if (exited) {
            // let the exit store itself commit (and the model reach it too)
            if (!opt.lockstep || iss.exited || cycle - exit_cycle > 64) {
                if (opt.lockstep && !iss.exited) {
                    std::fprintf(stderr, "vsim: the core wrote EXIT but the model did not\n");
                    status = 2;
                } else {
                    status = rv_exit_code(exit_value);
                }
            }
        } else if (cycle >= opt.max_cycles) {
            std::fprintf(stderr, "vsim: no exit after %" PRIu64 " cycles\n", cycle);
            status = 3;
        }
    }
    std::fflush(stdout);
    if (trace) std::fclose(trace);
    top->final();
#ifdef HAVE_PIPEVIEW
    pv.finish();
#endif

    if (exited) {
        if (opt.lockstep && status != 2 && std::string(iss.console ? iss.console : "", iss.console_len) != console) {
            std::fprintf(stderr, "vsim: console output differs from the golden model\n");
            status = 2;
        }
    }

    double cpi = retired ? (double)cycle / (double)retired : 0.0;
    if (opt.stats) {
        std::fprintf(stderr, "cycles %" PRIu64 "  instret %" PRIu64 "  CPI %.4f\n", cycle, retired, cpi);
        for (int e = 0; e < RV_HPM_COUNT; e++)
            std::fprintf(stderr, "  %-20s %" PRIu64 "\n", kEventNames[e], events[e]);
    }
    if (!opt.json.empty()) {
        FILE *f = std::fopen(opt.json.c_str(), "w");
        if (f) {
            std::fprintf(f, "{\"cycles\": %" PRIu64 ", \"instret\": %" PRIu64 ", \"commits\": %" PRIu64
                            ", \"cpi\": %.6f, \"exit\": %d",
                         cycle, retired, commits, cpi, status);
            for (int e = 0; e < RV_HPM_COUNT; e++)
                std::fprintf(f, ", \"%s\": %" PRIu64, kEventNames[e], events[e]);
            std::fprintf(f, "}\n");
            std::fclose(f);
        }
    }
    rv_iss_free(&iss);
    if (!exited || status == 2) return status;
    if (status != 0) {
        std::fprintf(stderr, "vsim: program exit code %d\n", status);
        return 1;
    }
    return 0;
}
