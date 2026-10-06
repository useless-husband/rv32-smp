#!/usr/bin/env python3
"""Synthesis resource and logic-depth report for the multicore, from Yosys'
estimate for the Spartan-7 XC7S50.  No place and route: the delay figure is a
logic-level estimate, not timing sign-off.

    python3 tools/synth_report.py       reads build/synth/stat{1,2,4}.json and
                                         build/synth/depth{1,2,4}.log, writes
                                         synth/report.md
"""
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SD = REPO / "build" / "synth"
# Spartan-7 XC7S50 (Xilinx DS180): 32,600 LUTs, 65,200 FFs, 75 x 36 Kb BRAM, 120 DSP.
CAP = {"LUT": 32600, "FF": 65200, "BRAM36": 75, "DSP": 120}
# A rough per-logic-level delay for Spartan-7 -1 speed grade (LUT + local route),
# used only to turn the logic depth into a ballpark ns figure.  Not sign-off.
NS_PER_LEVEL = 0.6


def area(path):
    j = json.loads(Path(path).read_text())
    cells = j["modules"][next(iter(j["modules"]))]["num_cells_by_type"]
    lut = sum(v for k, v in cells.items() if re.match(r"LUT[1-6]$", k))
    ff = sum(v for k, v in cells.items() if k.startswith("FD"))
    carry = cells.get("CARRY4", 0)
    bram = cells.get("RAMB36E1", 0) + cells.get("RAMB18E1", 0) * 0.5
    dsp = cells.get("DSP48E1", 0)
    lutram = sum(v for k, v in cells.items() if "RAM" in k and k.startswith("RAM"))
    return {"LUT": lut, "FF": ff, "CARRY4": carry, "BRAM36": bram, "DSP": dsp, "LUTRAM": lutram}


def depth(path):
    txt = Path(path).read_text()
    m = re.search(r"Longest topological path in \S+ \(length=(\d+)\)", txt)
    return int(m.group(1)) if m else None


def main():
    rows = {}
    for n in (1, 2, 4):
        s, d = SD / f"stat{n}.json", SD / f"depth{n}.log"
        if not s.exists():
            print(f"missing {s}; run make synth")
            return 1
        rows[n] = (area(s), depth(d) if d.exists() else None)

    out = ["# Synthesis estimate (Yosys, Spartan-7 XC7S50)\n",
           "`make synth`.  Yosys `synth_xilinx` maps the synthesisable multicore (synth/smp_core.sv:",
           "the cores and the snooping bus; memory is off-chip) to Spartan-7 primitives.  There is **no**",
           "place and route, so the delay is a logic-level estimate (depth x ~%.1f ns/level), not timing" % NS_PER_LEVEL,
           "sign-off.  Numbers measured on this machine.\n",
           "| harts | LUTs | (% of 32,600) | flip-flops | CARRY4 | block RAM (36 Kb) | DSP | logic depth | est. longest path |",
           "|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for n in (1, 2, 4):
        a, dep = rows[n]
        pct = 100.0 * a["LUT"] / CAP["LUT"]
        ns = f"{dep * NS_PER_LEVEL:.1f} ns" if dep else "n/a"
        out.append(f"| {n} | {a['LUT']:,} | {pct:.1f}% | {a['FF']:,} | {a['CARRY4']:,} | {a['BRAM36']:.0f} | "
                   f"{a['DSP']} | {dep} levels | {ns} |")
    a1 = rows[1][0]
    a2 = rows[2][0]
    out.append("")
    dl = a2["LUT"] - a1["LUT"]
    out.append(f"Each hart is one pipeline, its I-cache and MESI L1, and its share of the snoop logic; a hart "
               f"adds about {dl:,} LUTs and {a2['FF'] - a1['FF']:,} flip-flops.  Block RAM scales with the caches "
               "(one I-cache + one D-cache per hart).")
    fits = [n for n in (1, 2, 4) if rows[n][0]["LUT"] <= CAP["LUT"] and rows[n][0]["FF"] <= CAP["FF"]]
    big = [n for n in (1, 2, 4) if n not in fits]
    if fits:
        out.append(f"\n1 and 2 harts fit the XC7S50 (the largest Spartan-7).  " if fits == [1, 2] else "")
    if big:
        out.append(f"**{', '.join(map(str, big))} harts exceed the XC7S50's {CAP['LUT']:,} LUTs** "
                   "(43k for 4 harts): a 4-hart build needs a larger part (e.g. an Artix-7 XC7A100T, "
                   "63,400 LUTs, or a mid-size UltraScale).  Everything here is simulation and a Yosys "
                   "estimate; no board.")
    (REPO / "synth" / "report.md").write_text("\n".join(out) + "\n")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
