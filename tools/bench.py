#!/usr/bin/env python3
"""Run the parallel benchmarks (build/sw/bench.elf) on the 1-, 2- and 4-hart
builds and the false-sharing demo on the 4-hart build, and write a markdown
table of speedups and coherence traffic to build/bench.md.  All numbers are
simulated machine cycles measured on this machine; the method is in the
README.  Usage: python tools/bench.py [build-dir]"""
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build"


def run_json(sim, elf, *args):
    out = BUILD / f"{elf.stem}.{sim.name}.json"
    cmd = [str(sim), "--quiet", "--json", str(out), *args, str(elf)]
    subprocess.run(cmd, check=False, cwd=REPO, capture_output=True)
    return json.loads(out.read_text())


def run_console(sim, elf, *args):
    cmd = [str(sim), *args, str(elf)]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    return r.stdout


def main():
    bench = BUILD / "sw" / "bench.elf"
    rows = {}
    traffic = {}
    for n in (1, 2, 4):
        sim = BUILD / f"vsmp{n}"
        if not sim.exists():
            print(f"skip: {sim} missing")
            return
        line = run_console(sim, bench)
        m = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        j = run_json(sim, bench)
        rows[n] = m
        traffic[n] = j["bus"]

    lines = []
    lines.append("# Benchmark results\n")
    lines.append("Simulated machine cycles on this machine (Apple M5, shared), from the cores' own\n"
                 "`mcycle` counter.  Speed-up is cycles(1 hart) / cycles(n harts) for the same total\n"
                 "work; the checksum is identical across hart counts, so the work is really the same.\n")
    lines.append("\n## Mandelbrot (80x48, 50 iterations), rows split across harts\n")
    lines.append("| harts | cycles | speed-up | checksum | BusRd | BusRdX | BusUpgr | supplied by a cache |")
    lines.append("|---:|---:|---:|---:|---:|---:|---:|---:|")
    base = rows[1]["mandel_cycles"]
    for n in (1, 2, 4):
        c = rows[n]["mandel_cycles"]
        t = traffic[n]
        lines.append(f"| {n} | {c:,} | {base / c:.2f}x | {rows[n]['mandel_csum']} | {t['rd']} | {t['rdx']} | "
                     f"{t['upgr']} | {t['supplied']} |")
    lines.append("\n## Shared histogram (4096 samples, 16 atomically-updated buckets)\n")
    lines.append("A memory-sharing-heavy kernel: every bucket ping-pongs between caches.\n")
    lines.append("| harts | cycles | speed-up | bucket total |")
    lines.append("|---:|---:|---:|---:|")
    baseh = rows[1]["hist_cycles"]
    for n in (1, 2, 4):
        c = rows[n]["hist_cycles"]
        lines.append(f"| {n} | {c:,} | {baseh / c:.2f}x | {rows[n]['hist_total']} |")

    # false sharing
    fs = BUILD / "sw" / "false_sharing.elf"
    if fs.exists():
        out = run_console(BUILD / "vsmp4", fs)
        lines.append("\n## False sharing and its fix (4 harts, 20000 increments each)\n")
        lines.append("```\n" + out.strip() + "\n```\n")
        for n in (1, 2, 4):
            j = run_json(BUILD / f"vsmp{n}", fs)
    md = "\n".join(lines) + "\n"
    (BUILD / "bench.md").write_text(md)
    print(md)


if __name__ == "__main__":
    main()
