"""Randomised instruction streams (tests/random/rvgen.py) on the cores in
lockstep with the golden model: integer streams on all three, streams with F
and D instructions (rvgen.py --fp) on core B with the FPU.  Seeds are fixed:
1..RANDOM_SEEDS (default 100).  A failure prints the seed and the command
that reproduces it."""

import os
import subprocess
import sys

import pytest

from conftest import ALL_CORES, BUILD, FP_CORE, REPO, RVARCH, RVARCH_FD, assemble, run_vsim

SEEDS = range(1, int(os.environ.get("RANDOM_SEEDS", "100")) + 1)
LENGTH = int(os.environ.get("RANDOM_LENGTH", "3000"))


def build_program(seed, fp=False):
    out = BUILD / "random"
    out.mkdir(parents=True, exist_ok=True)
    stem = f"{'fp' if fp else ''}seed{seed}_n{LENGTH}"
    src, elf = out / f"{stem}.S", out / f"{stem}.elf"
    if not elf.exists():
        subprocess.run([sys.executable, str(REPO / "tests/random/rvgen.py"), "--seed", str(seed),
                        "--length", str(LENGTH), *(["--fp"] if fp else []), "-o", str(src)], check=True)
        assemble(src, elf, RVARCH_FD if fp else RVARCH)
    return elf


@pytest.mark.parametrize("core", ALL_CORES)
@pytest.mark.parametrize("seed", SEEDS)
def test_random_stream(core, seed):
    elf = build_program(seed)
    r = run_vsim(core, elf, "--max-cycles", "5000000")
    assert r.returncode == 0, (
        f"random seed {seed} failed on core {core} (exit {r.returncode}).\n"
        f"reproduce: make random-one SEED={seed} CORE={core}\n{r.stderr[-4000:]}")


@pytest.mark.parametrize("seed", SEEDS)
def test_random_fp_stream(seed):
    elf = build_program(seed, fp=True)
    r = run_vsim(FP_CORE, elf, "--max-cycles", "5000000")
    assert r.returncode == 0, (
        f"random FP seed {seed} failed on core {FP_CORE} (exit {r.returncode}).\n"
        f"reproduce: make random-one SEED={seed} CORE={FP_CORE} FP=1\n{r.stderr[-4000:]}")
