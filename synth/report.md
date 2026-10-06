# Synthesis estimate (Yosys, Spartan-7 XC7S50)

`make synth`.  Yosys `synth_xilinx` maps the synthesisable multicore (synth/smp_core.sv:
the cores and the snooping bus; memory is off-chip) to Spartan-7 primitives.  There is **no**
place and route, so the delay is a logic-level estimate (depth x ~0.6 ns/level), not timing
sign-off.  Numbers measured on this machine.

| harts | LUTs | (% of 32,600) | flip-flops | CARRY4 | block RAM (36 Kb) | DSP | logic depth | est. longest path |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 10,843 | 33.3% | 4,574 | 436 | 6 | 4 | 29 levels | 17.4 ns |
| 2 | 21,668 | 66.5% | 8,839 | 879 | 13 | 8 | 30 levels | 18.0 ns |
| 4 | 43,282 | 132.8% | 17,367 | 1,757 | 26 | 16 | 30 levels | 18.0 ns |

Each hart is one pipeline, its I-cache and MESI L1, and its share of the snoop logic; a hart adds about 10,825 LUTs and 4,265 flip-flops.  Block RAM scales with the caches (one I-cache + one D-cache per hart).

1 and 2 harts fit the XC7S50 (the largest Spartan-7).  
**4 harts exceed the XC7S50's 32,600 LUTs** (43k for 4 harts): a 4-hart build needs a larger part (e.g. an Artix-7 XC7A100T, 63,400 LUTs, or a mid-size UltraScale).  Everything here is simulation and a Yosys estimate; no board.
