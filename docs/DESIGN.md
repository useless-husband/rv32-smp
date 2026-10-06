# Design notes

How the multicore is built, the hard problems in the coherence protocol and
how they are solved, and the trade-offs that were rejected. The single-core
pipeline, caches, predictor, divider and golden ISS are from
[rv32-pipeline](../../自製RISC-V處理器%20rv32-pipeline) at commit `dcf1080`;
this document covers only what is new for the multicore.

## 1. Overview

```
        hart 0                 hart 1                 hart N-1
  +--------------+       +--------------+       +--------------+
  | pipeline     |       | pipeline     |  ...  | pipeline     |
  | (core.sv)    |       |              |       |              |
  | I$     L1D$  |       | I$     L1D$  |       | I$     L1D$  |
  +--+------+----+       +--+------+----+       +--+------+----+
     |      |               |      |               |      |
     | snoop| req           | snoop| req           |      |      (each cache is
     v      v               v      v               v      v       a bus master;
  ============================ snooping bus (bus.sv) =============  snoops go to
                     one transaction at a time                     every data cache
                              |                                    but the requester)
                              v
                       write-back memory
```

Each hart is `core.sv` (the rv32-pipeline five-stage core plus the A
extension, FENCE and `mhartid`) with a private instruction cache (`icache.sv`)
and a private MESI L1 data cache (`l1d.sv`). All caches sit on one snooping
bus (`bus.sv`) with write-back memory behind it. `rtl/sim/smp_top.sv`
instantiates `NHARTS` of them for simulation; `synth/smp_core.sv` is the same
without the simulation-only ports, for the Yosys estimate. Per-hart signals
are flat vectors (`sig[W*k +: W]`), not unpacked arrays of structs, so the RTL
stays in the subset Yosys 0.33 and Verilator 5.020 accept.

## 2. What changed in the core

* **A extension** (`rtl/decoder.sv`, `rtl/core.sv`). LR.W, SC.W and the nine
  AMOs decode as loads (they write `rd` from memory, so the existing load-use
  interlock already covers a dependent instruction) with `is_atomic` set and
  `amo_op = funct5`. The address is `rs1` with a zero offset. The actual
  read-modify-write happens in the L1 in the MEM stage (section 4), which is
  the single point where the core touches memory, so an atomic is naturally
  atomic with respect to the bus. `aq`/`rl` bits are accepted and need no
  action because the core is in-order with no store buffer (section 6).
  Atomics to non-cacheable (I/O) addresses trap.
* **`mhartid`** (`rtl/csr_file.sv`): CSR `0xF14` returns the `HARTID`
  parameter; the golden model returns the hart's id. `misa` advertises A.
* **More performance counters**: bus reads, read-for-ownership, upgrades,
  snoop invalidations, cache-to-cache supplies and SC failures, on top of the
  ten from rv32-pipeline.
* **Boot and `mhartid`-based stacks** (`sw/runtime/crt0.S`): every hart starts
  at the reset vector; hart 0 zeroes `.bss` and releases the others, each hart
  gets a 16 KiB stack below the top of RAM indexed by `mhartid`. The number of
  harts is read from an I/O register (`0x10000008`).

## 3. The bus and the coherence order

The bus serves **one transaction at a time** (an "atomic bus"). A transaction
is: arbitrate (round-robin over the 2N masters — N data caches then N
instruction caches), show the request to every data cache except the
requester's own (`snoop`), wait until each has answered, read memory if nobody
supplied the line, then acknowledge the requester with the line and a `shared`
flag. Commands are `BusRd` (read, others keep shared copies), `BusRdX` (read
for ownership, others invalidate), `BusUpgr` (shared→modified, no data),
`BusWB` (write back a dirty line), `IFetch` (instruction read, snooped so a
modified line in any data cache supplies it) and `Uncached` (I/O).

Because transactions never overlap, the order in which the bus grants them is
a total order on all coherence events, and it is exactly the coherence order
of every line. That is what makes the single-writer/multiple-reader argument
simple and what the golden memory checker relies on (section 7). The cost is
that all coherence traffic is serialised; a real machine would pipeline the
bus or use a directory. This is the main reason the core count is small.

**Rejected:** a directory. A directory scales to more cores and does not
serialise unrelated lines, but it needs per-line sharer state and a more
elaborate transient-state protocol (many more states to model and to get
right). For a first coherent multicore the snooping bus is far easier to build
*and to verify exhaustively* — the whole protocol is a few hundred lines and
the model checker covers 3 caches in under two seconds. The directory is noted
as future work.

## 4. The L1 data cache (`l1d.sv`)

Two ways, 16-byte lines, write-back, write-allocate, one LRU bit per set. The
MESI state of a line is three flip-flops (valid, exclusive, dirty: I = 0xx,
S = 100, E = 110, M = 111). Tags and data are synchronous-read arrays (block
RAM), as in rv32-pipeline; the state bits are flip-flops so snoops and resets
touch them in one cycle.

One controller multiplexes three jobs onto one array read port:

* **Core accesses** happen only in `IDLE`. A load hits in S/E/M. A store, LR,
  SC or AMO needs E or M; from E it goes to M silently (no bus traffic — the
  cache already has the only copy). An SC with no matching reservation fails at
  once, without the bus.
* **Misses** record the request (`BusRd` / `BusRdX` / `BusUpgr`), evict a
  victim (a dirty victim goes to the one-entry write-back buffer), and — this
  is the important part — **install the line and perform the waiting access in
  the same cycle the bus answers**. Doing the access at the moment of the fill
  means no other request can take the line away between the fill and its use,
  so two caches cannot livelock by repeatedly stealing a line from each other
  before either makes progress. The dirty victim is written back afterwards,
  from the buffer, while the core runs on; a new miss waits only if the buffer
  is still full.
* **Snoops** are served in `IDLE`, `REQ` (waiting for the bus) and `UNC` —
  never ignored, or two caches each waiting for the bus while refusing to
  snoop would deadlock. A snoop takes two cycles: latch the request and read
  the arrays, then answer (`snp_done`) with whether a copy is kept and whether
  a dirty line is supplied.

### The races the protocol has to resolve

These are the interesting part, and each is re-introduced as a bug variant
(section 9):

* **Upgrade losing its copy.** Cache A holds a line in S and asks for
  `BusUpgr` (modified, no data). Before the bus serves A, cache B's `BusRdX`
  or `BusUpgr` for the same line invalidates A's S copy. A's upgrade would now
  install a line it no longer has the data for. The fix: while an upgrade is
  outstanding, a snoop that invalidates its line turns the request into a
  `BusRdX`, so A re-reads the up-to-date data. (Bug 1 removes this.)
* **Request crossing a write-back.** A dirty line sits in the write-back
  buffer, not yet in memory. Another cache's `BusRd`/`BusRdX` for that line
  must be answered from the buffer — memory still has the old value. The fix:
  the buffer is snooped like a cache line and supplies the data (and is
  cancelled or downgraded). (Bug 2 removes this.)
* **Reservation vs remote store.** An LR sets a reservation on a 16-byte
  granule. Any remote write to that granule — seen as a snoop that invalidates
  the line — must clear the reservation so a later SC fails. (Bug 3 removes
  this.)
* **E vs a sharer.** A read miss may install E only if no other cache has the
  line; the bus's `shared` flag says whether one does. Taking E wrongly lets a
  later store upgrade silently with no invalidation, leaving stale sharers.
  (Bug 4 forces E.)
* **LR/SC forward progress.** If every remote snoop could invalidate a freshly
  reserved line, an LR/SC loop on a contended lock could livelock (the SC's
  line is stolen before the SC runs, forever). The L1 therefore **defers**
  snoops to a reserved line for a short window (`LOCKOUT` cycles) *while the
  core is not itself waiting on the cache*, which lets the SC usually succeed.
  The window must always close, or a hart that holds a reservation and then
  waits (say, on a flag another hart will set) would block that other hart
  forever. (Bug 5 makes the window never close → deadlock.)

## 5. Instruction fetch and `fence.i`

The instruction cache issues `IFetch` on a miss, which snoops the data caches:
a modified line in any hart's L1 supplies it. So self-modifying or
JIT-generated code works across harts: the writer does its stores and
`fence.i`; `fence.i` invalidates that hart's I-cache; the refetch snoops and
gets the modified data from whichever L1 holds it. The data cache needs no
flush on `fence.i` for this (unlike the single-core design, which wrote the
D-cache back): the snoop reaches the dirty line directly.

## 6. Memory model

The cores are in-order and perform every memory access — load, store, LR, SC,
AMO — in the MEM stage, one at a time, in program order; the L1 blocks the
pipeline until each one is done. There is **no store buffer and no
speculation past memory**. Consequently the machine is **sequentially
consistent**: there is a single global order (the interleaving of the harts'
in-order perform streams, consistent with the bus's coherence order) that
explains every value read. SC is far stronger than RISC-V's RVWMO memory model
requires.

This is a deliberate choice for a first coherent multicore: SC is the model
that is easy to reason about and to check (the golden memory checker in
section 7 is a direct SC checker). `FENCE` is therefore a no-op in hardware.
The software (`sw/runtime/smp.h`) still issues the architecturally required
fences and `aq`/`rl` bits, so it is correct on a weaker machine too; the
litmus tests (section 8) confirm the hardware never produces a relaxed outcome
even where RVWMO would allow one. A weaker, higher-performance implementation
(a store buffer, load speculation) is future work and would need the litmus
suite as its acceptance test.

## 7. Verification level 2: the golden memory checker (`sim/memcheck.h`)

Every L1 exports, each cycle, the access it performed: address, the old word
it read, the new word it wrote, and the kind (load/store/LR/SC/AMO). The
checker keeps a shadow of memory in perform order and, for each access,
requires:

* the old word the L1 saw equals the shadow's current word — i.e. every read
  returns the value of the latest write in perform order (and since that order
  is a legal total order of all accesses, every load value is *explained* by
  it);
* within one cycle, at most one cache writes a given line (two would mean two
  writable copies); a read and a write to one line in the same cycle are
  linearised read-before-write, which the atomic bus guarantees is sound;
* an SC succeeds only if no other hart wrote its reservation granule since this
  hart's LR (a shadow reservation per hart enforces the LR/SC atomicity).

A pass over long randomised runs is strong evidence of coherence *and* of
sequential consistency, because the order used is the hardware's own.

## 8. Verification level 1 and 3: model check and agreement

`model/mesi.hpp` is the protocol as an explicit state machine over abstract
caches, one bus and memory, with data abstracted to a single "fresh / stale"
bit per copy (exact for coherence: a store makes its copy the only fresh one,
copying data copies freshness). Its actions are the same as the RTL's —
access, grant, snoop, complete, write-back, lock-out timeout — and after every
action it checks single-writer/multiple-reader, the data-value invariant, and
a per-state progress property (a depth-first search over protocol-only actions
can always drain every outstanding request).

* **`model/mc.cpp`** does breadth-first reachability over this machine for
  2–3 caches and 1–2 addresses, with symmetry reduction: a state is stored
  under the smallest encoding over all renamings of caches and of addresses,
  which collapses the state space by roughly `n! · a!`. Breadth-first order
  means the first counterexample found for a bug variant is a shortest one; it
  is then replayed concretely to produce a readable trace and a JSON file for
  the HTML report.
* **The agreement check** (`mesi.hpp`'s `Agreement`, used by the harness with
  `--model`) maps each line address to a model address as it appears and
  replays every coherence event the RTL emits against the model: the model
  must allow the event, start from the state the RTL reports, and reach the
  state the RTL reaches. Events within a cycle are applied in an order the
  hardware guarantees is consistent (snoop answers, then fills, then accesses
  and misses, then bus grants). A transition the model forbids fails the run.

The three levels are independent: the model checker never runs the Verilog,
the memory checker never looks at MESI states, and the agreement check never
looks at data values. A real bug has to be consistent with all three to
survive, which (the mutation test shows) does not happen for one-line errors.

## 9. The bug museum

`rtl/rv_defs.svh` defines `BUG_*`; `l1d.sv` and `bus.sv` guard the exact line
that resolves each race with `BUG != BUG_x`. A simulator built with `-GBUG=k`
has bug k and nothing else. For each bug there is a directed two-hart program
(`tests/museum/`) that drives the race and a model-checker counterexample.
`tools/museum_data.py` collects both into `docs/museum.json`, which the HTML
report renders as a per-cache timeline. The five bugs and how each is caught
are in the README table; the point is that a plausible one-line "simplification"
of the protocol is caught by at least one of the three checkers, with a
concrete trace showing why.

## 10. Trade-offs rejected (summary)

* **Directory instead of snooping** — more scalable, much harder to verify
  exhaustively; deferred.
* **A weak (RVWMO) memory model with a store buffer** — higher performance,
  but needs the litmus suite as a correctness gate and a far larger model;
  deferred. The software is already written to be correct on it.
* **A shared L2** — would cut cache-to-cache traffic and raise the core count
  the snooping bus can sustain; deferred.
* **Committing a prebuilt `tla2tools.jar` and using TLC** — the machine has no
  Java runtime, and a hand-written explicit-state checker is small, fast
  enough here, and produces traces in exactly the form the report needs.
