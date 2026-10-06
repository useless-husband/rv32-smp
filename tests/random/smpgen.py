#!/usr/bin/env python3
"""Random multi-hart memory programs for the coherence stress test.

Each hart gets its own random stream of loads and stores of every width,
AMOs, LR/SC pairs, FENCEs and short delays, all aimed at a small pool of
shared lines chosen to make the protocol work hard:

  * lines 0-3   consecutive: several harts write different words of one line
                (false sharing, upgrade races)
  * lines 4-7   2 KiB apart, i.e. in the same L1 set as each other and as
                lines 0-3's set mates: a hart touching three of them evicts
                one, so write-backs cross other harts' requests
  * a private line per hart, also in a conflicting set

The program checks nothing itself: the harness checks every value read
against the golden memory (sim/memcheck.h), every instruction against each
hart's golden model, and (with --model) every coherence event against the
protocol model.  Output: one assembly file with body0..bodyN-1, linked with
tests/random/smpmain.c and the runtime.
"""
import argparse
import random

SET_STRIDE = 2048
AMOS = ["amoadd.w", "amoswap.w", "amoxor.w", "amoor.w", "amoand.w", "amomin.w", "amomax.w", "amominu.w",
        "amomaxu.w"]


def line_offsets(harts):
    """Byte offset of every pool line from the pool base."""
    offs = [16 * i for i in range(4)]                         # 0-3: one set each, consecutive
    offs += [SET_STRIDE * (i + 1) for i in range(4)]          # 4-7: same set as line 0
    offs += [SET_STRIDE * (5 + h) + 16 for h in range(harts)]  # private, same set as line 1
    return offs


def body(rng, hart, harts, length):
    offs = line_offsets(harts)
    shared = list(range(8))
    private = 8 + hart
    out = [f"    .globl body{hart}", f"body{hart}:", "    la t6, smp_pool"]
    ops = 0
    while ops < length:
        r = rng.random()
        line = private if rng.random() < 0.15 else rng.choice(shared)
        base = offs[line]
        word = rng.randrange(4)
        reg = rng.choice(["t0", "t1", "t2", "a0", "a1"])
        val = rng.randrange(-2048, 2048)
        if r < 0.30:                                       # loads of every width
            kind = rng.choice(["lw", "lw", "lh", "lhu", "lb", "lbu"])
            size =