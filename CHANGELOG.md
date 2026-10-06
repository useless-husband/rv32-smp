# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/); versions use semantic
versioning.

## [0.1.0] - 2026-10-06

First release: a cache-coherent multicore RISC-V, verified three ways.

### Added
- N-core RV32IMA (parameterised; built and tested at 1, 2 and 4 harts),
  starting from the rv32-pipeline five-stage core B (commit `dcf1080`) with the
  A extension (LR/SC, AMOs), FENCE and a per-hart `mhartid` CSR.
- Private MESI L1 data cache (`rtl/l1d.sv`): 2-way, write-back, write-allocate,
  one-entry write-back buffer, LR/SC reservation with a lock-out window, and
  the snoop logic that resolves the upgrade, write-back, reservation and
  shared-vs-exclusive races.
- Private instruction cache snooped on fetch, so `fence.i` works across harts.
- Snooping bus with a round-robin arbiter over the 2N masters and write-back
  memory (`rtl/bus.sv`); the atomic bus order is the coherence order.
- Abstract protocol model (`model/mesi.hpp`) and an explicit-state model
  checker with symmetry reduction (`model/mc.cpp`): all invariants hold for
  2-3 caches and 1-2 addresses; a shortest counterexample for every bug.
- Golden memory-order checker and RTL-vs-model agreement check in the Verilator
  harness, plus per-hart lockstep against one golden ISS per hart.
- Bug museum: five switchable protocol-bug variants, each with a directed
  two-hart program and a model counterexample.
- Multicore software: boot for all harts, fork/join, a barrier, spinlocks and
  ticket locks built with both AMOs and LR/SC; atomic-counter stress, random
  multi-core stress with timing perturbation, a false-sharing demo, and
  parallel benchmarks (Mandelbrot, shared histogram).
- Litmus tests (MP, SB, LB, 2+2W, IRIW and fence/AMO variants) checked against
  the reference RVWMO outcomes shipped with litmus-tests-riscv.
- Mutation test of the cache controller and bus (10 / 10 one-line mutants
  killed), Yosys resource estimate for 1/2/4 harts, a static HTML report, and
  the `跑跑看.command` launcher.
- CI on Ubuntu runners (Verilator 5.020, Yosys 0.33, LLVM 23): lint and loop
  check, model check, core correctness, and the bug museum plus litmus tests.
