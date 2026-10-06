#!/usr/bin/env python3
"""Mutation test for the coherence hardware: does the verification notice a
broken cache controller or bus?

Each mutant is a one-line change to rtl/l1d.sv, rtl/bus.sv or rtl/core.sv.
For every mutant the tool copies the sources into build/mutants/<id>/, applies
the change, rebuilds the 2- and 4-hart simulators and runs a short kill
battery: riscv-tests on each hart (lockstep), the atomic-counter and random
stress programs (lockstep + golden memory checker + protocol-model
agreement), and the bug-museum programs on the correct build.  A mutant is
killed when at least one of these exits nonzero (a wrong value, a memory-order
violation, a model disagreement, or a deadlock caught by a cycle budget).

    python3 tools/mutate.py            # all mutants, Markdown table on stdout
    python3 tools/mutate.py sc_always  # just one
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
WORK = REPO / "build" / "mutants"
COPY = ["Makefile", "rtl", "sim", "model", "sw", "tests"]

# id, file, original text, replacement, what it breaks
MUTANTS = [
    ("store_no_dirty", "rtl/l1d.sv",
     "if (writes_mem && !hway) dt0[idx] <= 1'b1;",
     "if (writes_mem && !hway) dt0[idx] <= 1'b0;",
     "a store hit does not mark the line dirty, so the write is lost on eviction"),
    ("snoop_no_inval", "rtl/l1d.sv",
     "                                if (!sway) v0[sidx] <= 1'b0;\n                                else v1[sidx] <= 1'b0;",
     "                                if (!sway) v0[sidx] <= v0[sidx];\n                                else v1[sidx] <= v1[sidx];",
     "a snooped BusRdX/BusUpgr does not invalidate the local copy (two writers)"),
    ("read_takes_e", "rtl/l1d.sv",
     "x0[idx] <= fcmd != `CMD_RD || !shared_eff;",
     "x0[idx] <= 1'b1;",
     "a read miss installs E in way 0 even when another cache shares the line"),
    ("sc_always", "rtl/l1d.sv",
     "assign sc_ok = is_sc && resv_match;",
     "assign sc_ok = is_sc;",
     "SC succeeds without a matching reservation (atomicity broken)"),
    ("resv_not_cleared", "rtl/l1d.sv",
     "                        if (s_inval && resv_hit && BUG != `BUG_LRSC_NO_CLEAR) begin\n                            resv_v <= 1'b0;",
     "                        if (s_inval && resv_hit && BUG != `BUG_LRSC_NO_CLEAR) begin\n                            resv_v <= resv_v;",
     "a remote store does not clear the LR reservation"),
    ("upgrade_not_converted", "rtl/l1d.sv",
     "                            BUG != `BUG_LOST_UPGRADE)\n                            fcmd <= `CMD_RDX;",
     "                            BUG != `BUG_LOST_UPGRADE)\n                            fcmd <= fcmd;",
     "a pending BusUpgr losing its copy is not turned into a BusRdX (lost upgrade)"),
    ("wb_not_snooped", "rtl/l1d.sv",
     "assign wbhit = (state == SNP) && wb_v && wb_addr[31:4] == sp_addr[31:4] && (BUG != `BUG_WB_NO_SNOOP);",
     "assign wbhit = 1'b0;",
     "the write-back buffer does not answer snoops (a request reads stale memory)"),
    ("rd_no_s", "rtl/l1d.sv",
     "if (l.st != I) { s.shared = 1; l.st = S; }",
     "if (l.st != I) { s.shared = 1; l.st = S; }",
     "(placeholder; replaced below)"),
    ("bus_no_shared", "rtl/bus.sv",
     "if (any_hit) shr <= 1'b1;",
     "if (any_hit) shr <= 1'b0;",
     "the bus never reports a line as shared, so readers take E (silent upgrade later)"),
    ("bus_no_snoop", "rtl/bus.sv",
     "        for (int k = 0; k < N; k++) snoopers[k] = (int'(win) != k);",
     "        for (int k = 0; k < N; k++) snoopers[k] = 1'b0;",
     "the bus snoops no other cache on a coherent request (no invalidations happen)"),
    ("atomic_not_load_use", "rtl/core.sv",
     "    assign load_use = e_valid && e_is_load && e_rd_we &&",
     "    assign load_use = e_valid && e_is_load && !e_is_atomic && e_rd_we &&",
     "an AMO/LR result is not interlocked against a dependent instruction"),
]
# drop the placeholder
MUTANTS = [m for m in MUTANTS if m[0] != "rd_no_s"]


def sh(cmd, cwd, timeout=240):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def battery(d):
    """Run the kill battery in mutant dir d; return (killed, which)."""
    env_make = ["make", "-s", "build/vsmp2", "build/vsmp4", "build/rvsim", "rvtests", "sw"]
    r = sh(env_make, d, timeout=400)
    if r.returncode != 0:
        return True, "build failed"
    B = d / "build"
    checks = []
    # single-core correctness on each hart
    for sim, nh in (("vsmp2", 2), ("vsmp4", 4)):
        for t in ("rv32ua-lrsc", "rv32ua-amoadd_w", "rv32ui-add"):
            for k in range(nh):
                checks.append((f"{t}@{sim}:h{k}", [str(B / sim), "--quiet", "--model", "--run-hart", str(k),
                                                   str(B / "rvtests" / f"{t}.elf")], 20))
    # multicore programs with all three checkers
    for sim in ("vsmp2", "vsmp4"):
        checks.append((f"atomics@{sim}", [str(B / sim), "--quiet", "--model", str(B / "sw" / "atomics.elf")], 60))
        for seed in (1, 3):
            checks.append((f"stress{seed}@{sim}", [str(B / sim), "--quiet", "--model", "--seed", str(seed),
                                                   "--stall-pct", "20", "--jitter", "8",
                                                   str(B / "sw" / "stress.elf")], 90))
    for prog, budget in (("lost_upgrade", "4000000"), ("wb_race", "4000000"), ("lrsc_clear", "4000000"),
                         ("e_shared", "4000000")):
        checks.append((f"{prog}@vsmp2", [str(B / "vsmp2"), "--quiet", "--model", "--max-cycles", budget,
                                         str(B / "sw" / f"{prog}.elf")], 120))
    for name, cmd, to in checks:
        try:
            r = sh(cmd, d, timeout=to)
        except subprocess.TimeoutExpired:
            return True, f"{name} (timeout: deadlock)"
        if r.returncode != 0:
            return True, name
    return False, ""


def apply_mut(d, f, old, new):
    p = d / f
    s = p.read_text()
    if s.count(old) != 1:
        return False
    p.write_text(s.replace(old, new))
    return True


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    muts = [m for m in MUTANTS if not only or m[0] == only]
    WORK.mkdir(parents=True, exist_ok=True)
    killed = 0
    rows = []
    for mid, f, old, new, desc in muts:
        d = WORK / mid
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
        for item in COPY:
            src = REPO / item
            dst = d / item
            if src.is_dir():
                shutil.copytree(src, dst)
            else:
                shutil.copy2(src, dst)
        if not apply_mut(d, f, old, new):
            rows.append((mid, desc, "SKIPPED (text not found or not unique)"))
            continue
        k, which = battery(d)
        if k:
            killed += 1
        rows.append((mid, desc, f"killed by {which}" if k else "**SURVIVED**"))
        print(f"{'KILL' if k else 'LIVE'}  {mid:22} {which}")
        sys.stdout.flush()
        shutil.rmtree(d)

    out = ["# Mutation test of the coherence hardware\n",
           f"{killed} / {len(rows)} mutants killed.\n",
           "| mutant | the bug it introduces | result |", "|---|---|---|"]
    for mid, desc, res in rows:
        out.append(f"| `{mid}` | {desc} | {res} |")
    md = "\n".join(out) + "\n"
    (REPO / "build" / "mutants.md").write_text(md)
    print("\n" + md)
    return 0 if killed == len([r for r in rows if "SKIPPED" not in r[2]]) else 1


if __name__ == "__main__":
    sys.exit(main())
