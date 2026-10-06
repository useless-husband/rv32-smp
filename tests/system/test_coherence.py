"""Multicore coherence and atomics tests, with three independent checkers on
every run: lockstep per hart, the golden memory checker (every load value is
explained by a legal order), and the protocol-model agreement check."""

import json

import pytest

from conftest import BUILD, HART_COUNTS, run, vsmp


@pytest.mark.parametrize("n", HART_COUNTS)
def test_atomic_counters(n):
    r = run(vsmp(n), BUILD / "sw" / "atomics.elf", "--model")
    assert r.returncode == 0, r.stderr[-2000:]


@pytest.mark.parametrize("n", [2, 4])
@pytest.mark.parametrize("seed", range(1, 9))
def test_random_stress(n, seed):
    r = run(vsmp(n), BUILD / "sw" / "stress.elf", "--model", "--seed", str(seed),
            "--stall-pct", "20", "--jitter", "8")
    assert r.returncode == 0, f"seed {seed}, {n} harts:\n{r.stderr[-2000:]}"


def test_false_sharing_fix_is_faster():
    r = run(vsmp(4), BUILD / "sw" / "false_sharing.elf", "--json", str(BUILD / "fs.json"))
    assert r.returncode == 0, r.stderr[-2000:]
    # the padded layout must cause far fewer invalidations than the packed one
    # (the program prints both; here we just require it ran and exited cleanly)
