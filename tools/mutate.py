#!/usr/bin/env python3
"""Mutation check: do the tests notice a broken design?

Each mutant is a one-line change to the RTL (or to the golden model).  For
every mutant the tool copies the sources into build/mutants/<id>/, applies
the change, rebuilds what the change touches and runs the test suites that
could notice it.  A mutant is "killed" when at least one suite fails.

    python3 tools/mutate.py            # all mutants, Markdown table on stdout
    python3 tools/mutate.py fwd_mem    # just one

Suites: unit = the module's cocotb test; rvtests = the riscv-tests in
lockstep (50 on the RV32IM cores, 71 with rv32uf/rv32ud on the FPU core
"pipe_fd"); random = random programs (seeds 1-30) in lockstep; random-fp =
random programs with F and D instructions (seeds 1-30, FPU core); perf = the
CoreMark efficiency bounds of tests/system/test_perf.py; iss = the
riscv-tests on the golden model alone; fpu = the FPU testbench (TestFloat
vectors without the long mulAdd runs, then 300,000 random operations);
testfloat = the golden model's arithmetic against the same TestFloat vectors.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
WORK = REPO / "build" / "mutants"
COPY = ["Makefile", "rtl", "sim", "model", "sw", "tests"]
SEEDS = range(1, 31)

# id, file, original text, replacement, what it breaks, unit test, cores
MUTANTS = [
    ("fwd_mem", "rtl/core_pipe.sv",
     "if (m_valid && m_rd_we && !m_is_load && m_rd == e_rs1) fwd1 = m_result;",
     "if (1'b0) fwd1 = m_result;",
     "no MEM->EX forwarding for rs1", None, ["pipe"]),
    ("fwd_wb", "rtl/core_pipe.sv",
     "else if (w_valid && w_rd_we && w_rd == e_rs2) fwd2 = w_result;",
     "else if (1'b0) fwd2 = w_result;",
     "no WB->EX forwarding for rs2", None, ["pipe"]),
    ("no_load_use", "rtl/core_pipe.sv",
     "assign load_use = e_valid && e_is_load && e_rd_we &&",
     "assign load_use = 1'b0 && e_valid && e_is_load && e_rd_we &&",
     "load-use hazard not detected (no stall)", None, ["pipe"]),
    ("no_id_flush", "rtl/core_pipe.sv",
     "if (rst || redirect_ex || redirect_mem) begin",
     "if (rst || redirect_mem) begin",
     "ID not flushed on a mispredict (wrong-path instruction executes)", None, ["pipe"]),
    ("no_operand_refresh", "rtl/core_pipe.sv",
     "e_rs1_val <= fwd1;\n            e_rs2_val <= fwd2;\n        end\n    end",
     "e_rs1_val <= e_rs1_val;\n            e_rs2_val <= fwd2;\n        end\n    end",
     "EX loses a forwarded rs1 while held by a D-cache miss", None, ["pipe"]),
    ("dirty_lost", "rtl/dcache.sv",
     "if (we && h0) dirty0[idx] <= 1'b1;",
     "if (we && h0) dirty0[idx] <= 1'b0;",
     "store hit in way 0 does not mark the line dirty (data lost on eviction)", "test_caches.py::test_dcache",
     ["pipe"]),
    ("no_store_bypass", "rtl/dcache.sv",
     "if (byp_v && byp_idx == idx && !byp_way) d0 =",
     "if (1'b0 && byp_idx == idx && !byp_way) d0 =",
     "load right after a store to the same line (way 0) reads stale data", "test_caches.py::test_dcache",
     ["pipe"]),
    ("lru_frozen", "rtl/dcache.sv",
     "lru[idx] <= !h1;",
     "lru[idx] <= lru[idx];",
     "LRU bit not updated on hits (performance bug only)", "test_caches.py::test_dcache", ["pipe"]),
    ("fencei_no_inval", "rtl/icache.sv",
     "valid <= '0;\n                    end else if",
     "valid <= valid;\n                    end else if",
     "FENCE.I does not invalidate the I-cache (stale code runs)", "test_caches.py::test_icache", ["pipe"]),
    ("bht_stuck", "rtl/bpred.sv",
     "if (upd_taken && bht[2*uh +: 2] != 2'b11) bht[2*uh +: 2] <= bht[2*uh +: 2] + 2'b01;",
     "if (upd_taken && bht[2*uh +: 2] != 2'b11) bht[2*uh +: 2] <= bht[2*uh +: 2];",
     "branch counters never move toward taken (performance bug only)", "test_bpred.py::test_bpred", ["pipe"]),
    ("sra_logical", "rtl/alu.sv",
     "`ALU_SRA:  y = $unsigned($signed(a) >>> sh);",
     "`ALU_SRA:  y = a >> sh;",
     "SRA/SRAI shift in zeros", "test_alu.py", ["single", "pipe"]),
    ("rem_sign", "rtl/divider.sv",
     "else if (is_rem) result = neg_r ? -r : r;",
     "else if (is_rem) result = r;",
     "REM result has the wrong sign for negative dividends", "test_muldiv.py::test_divider", ["pipe"]),
    ("branch_legal", "rtl/decoder.sv",
     "legal = (funct3 != 3'b010) && (funct3 != 3'b011);",
     "legal = 1'b1;",
     "reserved branch encodings accepted instead of trapping", "test_decoder.py", ["single", "pipe"]),
    ("mepc_unmasked", "rtl/csr_file.sv",
     "12'h341: mepc <= {wval[31:2], 2'b00};",
     "12'h341: mepc <= wval;",
     "mepc keeps the low two bits of a CSR write", "test_csr_file.py", ["single", "pipe"]),
    ("single_bltu", "rtl/core_single.sv",
     "3'b110: taken = (x1 < x2);",
     "3'b110: taken = ($signed(x1) < $signed(x2));",
     "single-cycle BLTU compares signed", None, ["single"]),
    ("model_mulhsu", "model/rv_iss.c",
     "case 2: return (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32);",
     "case 2: return (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32);",
     "golden model: MULHSU treats rs2 as signed", None, []),
    # ---- F and D: the pipeline around the FPU (lockstep on the FPU core)
    ("ffwd_mem", "rtl/core_pipe.sv",
     "if (m_valid && m_frd_we && !m_is_load && m_rd == e_rs1) ffwd1 = m_fresult;",
     "if (1'b0) ffwd1 = m_fresult;",
     "no MEM->EX forwarding for f register rs1", None, ["pipe_fd"]),
    ("ffwd_wb_rs3", "rtl/core_pipe.sv",
     "else if (w_valid && w_frd_we && w_rd == e_rs3) ffwd3 = w_fresult;",
     "else if (1'b0) ffwd3 = w_fresult;",
     "no WB->EX forwarding for the fused multiply-add's third operand", None, ["pipe_fd"]),
    ("no_fload_use", "rtl/core_pipe.sv",
     "assign fload_use = e_valid && e_is_load && e_frd_we &&",
     "assign fload_use = 1'b0 && e_valid && e_is_load && e_frd_we &&",
     "FLW/FLD followed by a use of that f register: no stall", None, ["pipe_fd"]),
    ("fsd_same_word", "rtl/core_pipe.sv",
     "m_addr <= m_addr4;",
     "m_addr <= m_addr;",
     "the second word of FLD/FSD goes to the first word's address", None, ["pipe_fd"]),
    ("fld_one_beat", "rtl/core_pipe.sv",
     "assign m_more = m_mem_dbl && !m_beat;",
     "assign m_more = 1'b0;",
     "FLD/FSD access only one word", None, ["pipe_fd"]),
    ("frm_ignored", "rtl/core_pipe.sv",
     "assign e_fp_rm = (e_funct3 == 3'b111) ? frm : e_funct3;",
     "assign e_fp_rm = e_funct3;",
     "dynamic rounding mode does not read frm", None, ["pipe_fd"]),
    ("fpu_no_kill", "rtl/core_pipe.sv",
     ".kill(redirect_mem), .ack(e_fire), .op(e_fp_op)",
     ".kill(1'b0), .ack(e_fire), .op(e_fp_op)",
     "an FPU operation flushed by FENCE.I keeps running (its result goes to the next instruction)",
     None, ["pipe_fd"]),
    ("fs_not_dirty", "rtl/csr_file.sv",
     "if (fp_dirty) fs <= 2'b11;",
     "if (fp_dirty) fs <= fs;",
     "mstatus.FS not set to dirty by a write to an f register", None, ["pipe_fd"]),
    ("fflags_not_sticky", "rtl/csr_file.sv",
     "fflags <= fflags | fp_flags;",
     "fflags <= fp_flags;",
     "fflags overwritten instead of accumulated", None, ["pipe_fd"]),
    ("fsqrt_rs2", "rtl/fp_decoder.sv",
     "fop_ok = (rs2 == 5'd0); fwrites = 1'b1; rm_used = 1'b1; uses_frs1 = 1'b1;",
     "fop_ok = 1'b1; fwrites = 1'b1; rm_used = 1'b1; uses_frs1 = 1'b1;",
     "FSQRT with a nonzero rs2 field accepted instead of trapping", None, ["pipe_fd"]),
    # ---- F and D: inside the FPU (unit testbench)
    ("rmm_as_rne", "rtl/fp_roundup.sv",
     "default: up = g;",
     "default: up = g && (s || lsb);",
     "round-to-nearest-max-magnitude breaks ties to even", "fpu", []),
    ("align_sticky", "rtl/fp_fma.sv",
     "aw_st <= (|sh_out[52:0]);",
     "aw_st <= 1'b0;",
     "addend bits shifted out of the adder window are forgotten", "fpu", []),
    ("fma_negative", "rtl/fp_fma.sv",
     "sum <= neg_c ? ~r0[163:0] : pos[163:0];",
     "sum <= pos[163:0];",
     "a negative difference is not turned back into a magnitude", "fpu", []),
    ("tiny_before", "rtl/fp_round.sv",
     "assign tiny = below && !((exp == emin - 13'sd1) && ones_u && up_u);",
     "assign tiny = below;",
     "tininess detected before rounding (the RISC-V rule is after)", "fpu", []),
    ("nan_unboxed", "rtl/fp_unpack.sv",
     "assign s32 = (&x[63:32]) ? x[31:0] : 32'h7fc0_0000;",
     "assign s32 = x[31:0];",
     "a single that is not NaN-boxed is used as it is", "fpu", []),
    ("f2i_limit", "rtl/fp_f2i.sv",
     "else range_bad = sign ? (mag > 33'h0_8000_0000) : (mag > 33'h0_7fff_ffff);",
     "else range_bad = (mag > 33'h0_8000_0000);",
     "FCVT.W accepts +2^31", "fpu", []),
    ("fmin_zero", "rtl/fp_misc.sv",
     "assign a_less = lt || (both_zero && sa && !sb);",
     "assign a_less = lt;",
     "FMIN/FMAX do not order -0 below +0", "fpu", []),
    ("sqrt_odd_exp", "rtl/fp_divsqrt.sv",
     "d <= ea[0] ? {sa, 1'b0} : {1'b0, sa};",
     "d <= {1'b0, sa};",
     "square root ignores an odd exponent", "fpu", []),
    ("div_sticky", "rtl/fp_divsqrt.sv",
     "assign sticky = (rem != 60'd0) || (x[55] && x[0]);",
     "assign sticky = (x[55] && x[0]);",
     "divide/sqrt drop the remainder (inexact results look exact)", "fpu", []),
    ("inf_times_zero", "rtl/fpu.sv",
     "spec_nv = any_snan || inf_x_zero || (!any_nan && prod_inf && r_inf && (p_s != r_s));",
     "spec_nv = any_snan || (!any_nan && prod_inf && r_inf && (p_s != r_s));",
     "infinity times zero is not an invalid operation", "fpu", []),
    ("model_rne", "model/rv_fp.c",
     "case RV_RNE: return g && (s || lsb);",
     "case RV_RNE: return g;",
     "golden model: round-to-nearest-even breaks ties away from zero", None, []),
]


def sh(cmd, cwd, timeout=900):
    return subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True, timeout=timeout)


def prepare(mid, path, old, new):
    d = WORK / mid
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    for item in COPY:
        src = REPO / item
        (shutil.copytree if src.is_dir() else shutil.copy)(src, d / item)
    (d / "build").mkdir()
    for sub in ("rvtests", "random", "third_party", "sw"):
        if (REPO / "build" / sub).exists():
            os.symlink(REPO / "build" / sub, d / "build" / sub)
    f = d / path
    text = f.read_text()
    assert text.count(old) == 1, f"{mid}: original text must occur exactly once in {path}"
    f.write_text(text.replace(old, new))
    return d


TFGEN = REPO / "build/third_party/berkeley-testfloat-3/build/Linux-x86_64-GCC/testfloat_gen"


def testfloat(d, checker):
    """The TestFloat vectors (without the long mulAdd runs) through build/<checker>."""
    r = sh(f"make -s build/{checker}", d)
    if r.returncode:
        return True
    r = sh(f'"{sys.executable}" tests/fp/testfloat.py --gen "{TFGEN}" --check build/{checker} --skip mulAdd', d)
    return r.returncode != 0


def run_suites(d, unit, cores, path):
    res = {}
    env = f'PYTHON="{sys.executable}"'
    if unit == "fpu":
        bad = testfloat(d, "fpu_tb")
        if not bad:
            bad = sh("./build/fpu_tb --random 300000 1", d).returncode != 0
        res["fpu"] = bad
    elif unit:
        r = sh(f'{env} "{sys.executable}" -m pytest -q -x "tests/unit/{unit}"', d)
        res["unit"] = r.returncode != 0
    if path.startswith("model/"):
        r = sh("make -s iss-test", d)
        res["iss"] = r.returncode != 0
    if path == "model/rv_fp.c":
        res["testfloat"] = testfloat(d, "fp_check")
    for core in cores:
        r = sh(f"make -s build/vsim_{core}", d)
        if r.returncode:
            res[f"build {core}"] = True
            continue
        fails = 0
        # the F and D tests only make sense on the core with the FPU
        pattern = "*.elf" if core == "pipe_fd" else "rv32u[im]-*.elf"
        for elf in sorted((REPO / "build" / "rvtests").glob(pattern)):
            p = sh(f'./build/vsim_{core} --quiet --max-cycles 2000000 "{elf}"', d, timeout=120)
            fails += p.returncode != 0
        res[f"rvtests {core}"] = fails
        fails = 0
        for seed in SEEDS:
            elf = REPO / "build" / "random" / f"seed{seed}_n3000.elf"
            p = sh(f'./build/vsim_{core} --quiet --max-cycles 5000000 "{elf}"', d, timeout=120)
            fails += p.returncode != 0
        res[f"random {core}"] = fails
        if core == "pipe_fd":
            fails = 0
            for seed in SEEDS:
                elf = REPO / "build" / "random" / f"fpseed{seed}_n3000.elf"
                p = sh(f'./build/vsim_{core} --quiet --max-cycles 5000000 "{elf}"', d, timeout=120)
                fails += p.returncode != 0
            res[f"random-fp {core}"] = fails
        if core == "pipe":
            r = sh(f'"{sys.executable}" -m pytest -q tests/system/test_perf.py', d)
            res["perf pipe"] = r.returncode != 0
    return res


def main():
    pick = set(sys.argv[1:])
    missing = [s for s in SEEDS if not (REPO / "build" / "random" / f"seed{s}_n3000.elf").exists()
               or not (REPO / "build" / "random" / f"fpseed{s}_n3000.elf").exists()]
    if missing or not (REPO / "build" / "rvtests").exists():
        sys.exit("run 'make system' first (it builds the riscv-tests and the random programs)")
    if not TFGEN.exists():
        sys.exit("run 'make fpu-unit' first (it builds TestFloat's vector generator)")
    rows = []
    for mid, path, old, new, what, unit, cores in MUTANTS:
        if pick and mid not in pick:
            continue
        d = prepare(mid, path, old, new)
        res = run_suites(d, unit, cores, path)
        killed = any(bool(v) for v in res.values())
        detail = ", ".join(f"{k}: {'FAIL' if v is True else (str(v) + ' failed') if v else 'pass'}"
                           for k, v in res.items())
        rows.append((mid, path, what, "killed" if killed else "SURVIVED", detail))
        print(f"{mid:20s} {'killed' if killed else 'SURVIVED':8s} {detail}", file=sys.stderr, flush=True)
        shutil.rmtree(d, ignore_errors=True)
    print("| Mutant | File | What it breaks | Result | Suites (failing runs) |")
    print("|---|---|---|---|---|")
    for mid, path, what, verdict, detail in rows:
        print(f"| `{mid}` | `{path}` | {what} | {verdict} | {detail} |")
    if any(r[3] != "killed" for r in rows):
        sys.exit(1)


if __name__ == "__main__":
    main()
