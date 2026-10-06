"""Shared helpers for the multicore system tests.  `make system` builds the
simulators (build/vsmpN and the bug variants), the golden model, the
riscv-tests and the demo/stress/litmus programs first; the Makefile passes
the cross-compiler paths in CLANG and LLD."""

import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
HART_COUNTS = [1, 2, 4]
BUGS = [1, 2, 3, 4, 5]


def vsmp(n, bug=None):
    name = f"vsmp{n}" if bug is None else f"vsmp{n}-bug{bug}"
    path = BUILD / name
    if not path.exists():
        pytest.skip(f"{path} not built (run make sims)")
    return path


def run(sim, elf, *args, timeout=900):
    cmd = [str(sim), "--quiet", *map(str, args), str(elf)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO)
