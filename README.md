# rv32-smp

**A cache-coherent multicore RISC-V: N in-order RV32IMA cores (tested at 1, 2
and 4), each with a private MESI L1 data and instruction cache on a snooping
bus, built on top of a single-core pipeline. The coherence protocol is
verified three ways that agree with each other — an exhaustive model check, a
golden memory-order checker on the running RTL, and step-by-step agreement
between the RTL and an abstract model — and four classic coherence/atomics
bugs are reproduced end to end, each caught by that verification.**

This is a learning extension of the author's own single-core processor,
[rv32-pipeline](../自製RISC-V處理器%20rv32-pipeline) (it starts from commit
`dcf1080`: the five-stage RV32IM core B, its caches, branch predictor, golden
ISS, riscv-tests harness and Yosys flow are copied in; the floating-point unit
and the single-cycle core A are dropped). Multicore RISC-V and cache-coherence
simulators are common course and open-source projects — see
[Related work](#related-work). Nothing here is a new idea; the point is that
the protocol is verified at three levels that have to agree, and that known
protocol bugs are shown breaking, in the model and in the hardware, so you can
watch *why* each one breaks. Everything runs in simulation; there is **no**
FPGA board, only a Yosys resource estimate.

[繁體中文說明](README.zh-TW.md) · [Design notes](docs/DESIGN.md) ·
[初學者導讀](docs/導讀.zh-TW.md) · [HTML report](docs/report.html) (open in a browser)

## What is in it

* **`rtl/core.sv`** — one hart: the five-stage pipeline (IF/ID/EX/MEM/WB) with
  forwarding, a load-use interlock, a BTB + 2-bit-counter branch predictor and
  an iterative divider, from rv32-pipeline, extended with the **A extension**
  (LR.W, SC.W and the nine AMOs), FENCE, and a per-hart `mhartid` CSR.
* **`rtl/l1d.sv`** — the private L1 data cache: 2-way, 16-byte lines, write-back,
  write-allocate, with the **MESI** protocol, a one-entry write-back buffer,
  an LR/SC reservation (with a short lock-out window so LR/SC loops make
  progress), and the snoop logic. It resolves the races a snooping protocol
  has to: an upgrade that loses its copy becomes a read-for-ownership, a
  request that arrives while a line sits in the write-back buffer is answered
  from the buffer, a remote store clears a reservation.
* **`rtl/bus.sv`** — the snooping bus: one transaction at a time, round-robin
  arbitration over the 2N masters (N data caches, N instruction caches),
  write-back memory behind it. Because transactions do not overlap, the bus
  order *is* the coherence order of every line.
* **`rtl/icache.sv`** — the instruction cache; instruction fetches snoop the
  data caches (an `IFetch` bus command), so a store followed by `fence.i` on
  any hart is seen by every hart's fetch.
* **`model/mesi.hpp`** — the abstract protocol, used both by the model checker
  and by the RTL agreement check.
* **`model/mc.cpp`** — an explicit-state model checker with symmetry reduction.
* **`model/rv_iss.c`** — the golden instruction-set model (RV32IMA), one
  instance per hart, from rv32-pipeline with the A extension and `mhartid`
  added.
* **`sim/`** — the Verilator harness: the golden memory-order checker
  (`memcheck.h`), the RTL↔model agreement check, and per-hart lockstep.
* **`sw/runtime/`** — the bare-metal runtime with boot for all harts, fork/join
  (`rt_run_all`), a barrier, spinlocks and ticket locks built with both AMOs
  and LR/SC.

## Results

All numbers measured on this machine (Apple M5, macOS, shared with other
jobs; the cycle counts are from the cores' own counters, so load does not
change them), with the commands shown. `make test` runs the lint, the model
check, the golden-model ISA tests and the full system suite in about two
minutes.

| What | Command | Result |
|---|---|---|
| Explicit-state model check of the exact protocol, 2–3 caches × 1–2 addresses: single-writer/multiple-reader, data-value and deadlock-freedom | `make mc` | all properties hold; largest 3 caches × 2 addresses = **500,087 states** in 1.6 s |
| Each bug variant has a shortest counterexample | `make mc` | 5 / 5 found |
| Official riscv-tests (rv32ui + rv32um + rv32ua) on **every hart** of the 1-, 2- and 4-hart builds, in lockstep with that hart's golden model and with the RTL↔model agreement check on | `pytest tests/system/test_riscv_tests.py` | 60 tests × (1+2+4) harts = **420 / 420 pass** |
| Atomic-counter stress (6 lock/counter kinds) on 1/2/4 harts, three checkers | `pytest -k coherence` | pass; no lost update |
| Random multi-core stress, fixed seeds, timing perturbed, golden memory checker + model agreement | `pytest -k stress` | pass (seeds 1–8 × 2/4 harts) |
| Litmus tests (MP, SB, LB, 2+2W, IRIW, fence and AMO variants), 3,000 runs each, every observed outcome checked against the reference RVWMO model shipped with the suite | `make litmus` | **0 relaxed outcomes** — sequentially consistent, stronger than RVWMO; every outcome allowed |
| Bug museum: each bug reproduced in the RTL by a directed program, and passing on the correct build | `pytest -k museum` | 5 / 5 caught, 5 / 5 pass when correct |
| Mutation test of the cache controller and bus (one-line RTL changes) | `python tools/mutate.py` | **10 / 10 mutants killed** |
| Verilator `-Wall` on 1/2/4 harts and every bug variant | `make lint` | clean |
| `yosys check -assert` (no combinational loops, no multiple drivers), 1/2/4 harts | `make loopcheck` | clean |

### Speed-up (from `make bench`, full table in [docs/benchmarks.md](docs/benchmarks.md))

Parallel Mandelbrot (rows split across harts; the checksum is identical for
1, 2 and 4 harts, so the work really is the same):

| harts | cycles | speed-up | BusRdX | lines supplied cache-to-cache |
|---:|---:|---:|---:|---:|
| 1 | 1,692,930 | 1.00× | 37 | 0 |
| 2 | 852,072 | 1.99× | 2,233 | 2,209 |
| 4 | 429,560 | 3.94× | 3,093 | 3,077 |

A memory-sharing-heavy kernel (every hart bumps a shared 16-bucket histogram
with `amoadd`) scales far less — 1.46× on 2 harts, 2.02× on 4 — because the
buckets ping-pong between caches. A **false-sharing** demo (each hart writes
its own counter, but packed into one cache line) is **2.99× slower** than the
padded version that puts each counter on its own line; `make bench` prints
both and the invalidation counts.

### Synthesis estimate (Yosys, Spartan-7 XC7S50; no place and route)

| harts | LUTs | flip-flops | block RAM (36 Kb) | DSP | logic depth |
|---:|---:|---:|---:|---:|---:|
| 1 | 10,843 (33%) | 4,574 | 6 | 4 | 29 levels |
| 2 | 21,668 (66%) | 8,839 | 13 | 8 | 30 levels |
| 4 | 43,282 | 17,367 | 26 | 16 | 30 levels |

1 and 2 harts fit the XC7S50 (the largest Spartan-7); **4 harts do not** (43k
LUTs > 32,600) and would need a larger part. The delay column is a rough
logic-level count, **not** timing sign-off. See
[synth/report.md](synth/report.md).

## How the three levels of verification fit together

1. **Model check (offline, exhaustive).** `model/mesi.hpp` is the protocol as
   a state machine over abstract caches, a bus and memory (data abstracted to
   "fresh / stale"). `model/mc.cpp` explores every reachable state for small
   configurations, with symmetry reduction over caches and addresses, and
   checks single-writer/multiple-reader, the data-value invariant and progress
   (the protocol alone can always finish an outstanding request). Breadth-first
   order makes the first counterexample a shortest one.
2. **Golden memory checker (on every RTL run).** The RTL reports every access
   each L1 performs (address, old word, new word) in the cycle it happens. The
   checker (`sim/memcheck.h`) keeps a shadow memory in that perform order and
   requires that every read returns the latest write in the order, that no two
   caches perform conflicting writes to one line in the same cycle, and that an
   SC succeeds only if no other hart wrote its reservation granule since the
   LR. The perform order is the hardware's own, so a pass is also a witness
   that the machine is sequentially consistent.
3. **RTL ↔ model agreement (on every RTL run with `--model`).** Every
   coherence event the RTL produces — a miss, a bus grant, a snoop answer, a
   fill, a write-back — is replayed against `mesi.hpp`, which must allow it and
   reach the same line state. A transition the model forbids, or a different
   resulting state, fails the run.

Three independent checkers that agree is the point: a bug has to fool all
three. The [bug museum](#bug-museum) shows what happens when it cannot.

## Bug museum

Five classic coherence/atomics bugs, each a switchable RTL variant (`-GBUG=k`,
see `rtl/rv_defs.svh`) and a directed two-hart program. For each one the model
checker gives the shortest counterexample, the RTL build reproduces it, and
the correct build passes. The [HTML report](docs/report.html) renders each as a
timeline you can read.

| # | bug | caught in the RTL by |
|---|---|---|
| 1 | **lost upgrade**: a pending BusUpgr that loses its shared copy is not re-issued as BusRdX | golden memory checker (stale read) |
| 2 | **write-back race**: the write-back buffer is not snooped, so a request reads stale memory | golden memory checker (stale read) |
| 3 | **LR/SC**: a remote store does not clear the reservation, so SC wrongly succeeds | golden memory checker (SC atomicity) |
| 4 | **silent E**: a read miss takes Exclusive although another cache has the line | RTL↔model disagreement (single-writer/multiple-reader) |
| 5 | **lock-out forever**: the LR lock-out window never closes | deadlock (no progress), caught by a cycle budget; the model checker's progress check also flags it |

## Demo

Double-click `跑跑看.command`, or:

```
$ make -j4 build/vsmp4 build/sw/atomics.elf
$ ./build/vsmp4 --model --stats build/sw/atomics.elf
amoadd               800 / 800
...
atomics: ok on 4 harts
  bus: BusRd 4877  BusRdX 4030  BusUpgr 4826 ... supplied-by-cache 8823
  memory check: 33980 accesses, 11539 writes, SC 2400 ok / 2 failed
  model agreement: 102645 coherence events checked
```

A deliberately broken build (bug 1) caught end to end:

```
$ ./build/vsmp2-bug1 --model build/sw/lost_upgrade.elf
MEMORY CHECK FAILED: cycle 7263: hart 0 load at 80001754 saw 00000018,
  but the latest write in perform order left 00000019 (hart 1 at cycle 7213)
```

## Build and test

Needs Verilator 5, Yosys, clang with the RISC-V target and ld.lld (Homebrew
LLVM on macOS, since Apple's clang has no RISC-V target), Python 3.10+ and a
C++17 compiler. The first run fetches riscv-tests at a pinned commit into
`build/third_party`; `make litmus` additionally clones litmus-tests-riscv into
the gitignored `data/`.

```sh
brew install verilator yosys llvm lld      # macOS
make test        # lint + loopcheck + model check + ISA tests + system suite (~2 min)
make mc          # the model checker alone (writes build/mc.md)
make bench       # speed-ups and coherence traffic (writes build/bench.md)
make litmus      # litmus tests vs the reference RVWMO model
make museum      # regenerate the bug-museum data
make report      # the static HTML report -> docs/report.html
make synth       # Yosys resource estimate for 1/2/4 harts -> synth/report.md
python tools/mutate.py           # mutation test of the cache controller and bus
make NHARTS=... not needed: the hart count is baked into each simulator (vsmp1/2/4)
```

Run every target from the repository root; paths may contain spaces or
non-ASCII characters, so all paths in the Makefile are relative.

## Repository layout

```
rtl/            decoder/ALU/regfile/CSR/divider/predictor (from rv32-pipeline), and the new
rtl/core.sv     one hart (pipeline + A extension + mhartid)
rtl/l1d.sv      private L1 data cache with MESI, write-back buffer, LR/SC
rtl/bus.sv      snooping bus, arbiter, write-back memory interface
rtl/icache.sv   instruction cache (snooped on fetch)
rtl/sim/        simulation top (N cores + bus + memory) and the memory model
model/mesi.hpp  the abstract protocol (shared by the checker and the agreement check)
model/mc.cpp    explicit-state model checker
model/rv_iss.c  golden ISS (RV32IMA), one per hart
sim/            Verilator harness: memory checker, model agreement, lockstep
sw/runtime/     boot for all harts, fork/join, barrier, spin/ticket locks (AMO and LR/SC)
tests/programs/ atomics, random stress, false sharing, benchmarks, litmus
tests/museum/   the five bug-reproduction programs
tests/system/   pytest suites
tools/          bench, synthesis report, mutation test, museum data, HTML report
```

## Limitations

* Simulation only. No board bring-up, no timing closure; the Yosys delay
  figure is a logic-level estimate, not sign-off. A 4-hart build does not fit
  the XC7S50.
* The cores are in-order and perform every memory access in program order at
  one point (no store buffer), so the machine is **sequentially consistent** —
  stronger than RVWMO requires. FENCE is therefore a no-op in the hardware;
  the software still uses the correct fences so it is portable to a weaker
  machine.
* RV32IMA only: no FPU (it is in rv32-pipeline), no supervisor mode, no
  interrupts, no MMU, no compressed instructions, no `Zacas` (`amocas`).
* One cache level (private L1s); no shared L2, no directory — the snooping bus
  is the whole interconnect, which is why the core count is small. A directory
  and an L2 are future work.
* The model checker proves the *abstract* protocol for small configurations
  (up to 3 caches, 2 addresses); it is a strong check of the design, not a
  proof of the Verilog. The RTL↔model agreement check connects the two on real
  runs but is not a formal equivalence proof.
* The atomic snooping bus serialises all coherence traffic; a real design would
  pipeline or split it.

## Related work

Multicore RISC-V cores and cache-coherence simulators are common; this is a
learning project that stands next to, not ahead of, them.

* **MIT 6.004 / 6.175 / 6.191 and 6.5900 (CS 252-style) labs**: multicore and
  coherence are standard material; this follows that progression in
  SystemVerilog.
* **CMU 15-418 "Snooping-based coherence simulator" / "multi-core cache
  simulator"**: course projects that model MSI/MESI over a bus, as the
  `model/` here does — but as a cycle-driven simulator, not driving real RTL.
* **gem5's Ruby / SLICC**: coherence protocols specified and checked, at far
  greater scope and maturity.
* **OpenPiton, BlackParrot, Rocket/BOOM + TileLink, CVA6 multicore**: real,
  synthesizable multicore RISC-V with directory or TileLink coherence. They are
  production-scale; this is a few-hundred-line snooping L1 built to be read and
  verified.
* **The RISC-V litmus tests (litmus-tests-riscv) and the RVWMO operational
  model** (Flur, Sarkar, Sewell et al.): the memory-model test suite and
  reference outcomes used here. This core is sequentially consistent, so it
  trivially satisfies RVWMO; the tests confirm it and show it is stronger.
* **TLA+/Murφ MESI models**: textbook coherence-protocol model checking, which
  `model/mc.cpp` is a small hand-written instance of (an explicit-state search
  with symmetry reduction).

Numbers from other cores were measured with other tools and run rules, so they
are context, not a ranking.

## License

MIT (see [LICENSE](LICENSE)). riscv-tests (BSD-style, Regents of the University
of California) is fetched at build time and is not part of this repository; the
litmus-tests-riscv suite (fetched by `make litmus` into the gitignored `data/`)
is under its own licence and is likewise not redistributed here.
