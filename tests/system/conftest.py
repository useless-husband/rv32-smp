"""Shared helpers for the system tests.  They expect `make system` (or
`make test`) to have built the simulators, the golden model, the
riscv-tests and the demo programs; the Makefile passes the cross-compiler
paths in CLANG and LLD."""

import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
CORES = ["single", "pipe"]          # RV32IM: core A and core B
FP_CORE = "pipe_fd"                 # core B built with FPU=1 (RV32IMFD)
ALL_CORES = CORES + [FP_CORE]
RVARCH = ["--target=riscv32-unknown-elf", "-march=rv32im_zicsr_zifencei", "-mabi=ilp32"]
RVARCH_FD = ["--target=riscv32-unknown-elf", "-march=rv32imfd_zicsr_zifencei", "-mabi=ilp32"]


def vsim(core):
    path = BUILD / f"vsim_{core}"
    if not path.exists():
        pytest.skip(f"{path} not built (run make system)")
    return path


def run_vsim(core, elf, *args, timeout=600):
    """Run a program in lockstep; returns CompletedProcess (text)."""
    cmd = [str(vsim(core)), "--quiet", *map(str, args), str(elf)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO)


def assemble(src, elf, arch=RVARCH):
    """Assemble one .S file with the platform header and link it."""
    clang = os.environ.get("CLANG", "clang")
    lld = os.environ.get("LLD", "ld.lld")
    obj = Path(str(elf) + ".o")
    subprocess.run([clang, *arch, f"-I{REPO / 'model'}", "-c", "-o", str(obj), str(src)], check=True)
    subprocess.run([lld, "-T", str(REPO / "sw/runtime/link.ld"), "-o", str(elf), str(obj)], check=True)
