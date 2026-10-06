"""The official riscv-tests on the cores, each run in lockstep with the golden
model: rv32ui and rv32um on core A, core B and core B with the FPU; rv32uf
and rv32ud on core B with the FPU.  A test passes when it reports success
through the EXIT register AND every committed instruction matched."""

import re

import pytest

from conftest import ALL_CORES, BUILD, FP_CORE, REPO, run_vsim

RISCV_TESTS = BUILD / "third_party" / "riscv-tests"
EXTS = {"rv32ui": "RV32UI", "rv32um": "RV32UM", "rv32uf": "RV32UF", "rv32ud": "RV32UD"}


def makefile_list(var):
    text = (REPO / "Makefile").read_text()
    m = re.search(rf"^{var} := ((?:.*\\\n)*.*)$", text, re.M)
    return m.group(1).replace("\\\n", " ").split()


INT_TESTS = [f"{e}-{t}" for e in ("rv32ui", "rv32um") for t in makefile_list(EXTS[e])]
FP_TESTS = [f"{e}-{t}" for e in ("rv32uf", "rv32ud") for t in makefile_list(EXTS[e])]


def upstream_list(ext):
    frag = (RISCV_TESTS / "isa" / ext / "Makefrag").read_text()
    m = re.search(rf"^{ext}_sc_tests = \\\n((?:.*\\\n)*)", frag, re.M)
    return m.group(1).replace("\\\n", " ").split()


@pytest.mark.parametrize("ext", list(EXTS))
def test_list_matches_upstream(ext):
    """Our Makefile runs exactly the tests the pinned commit lists."""
    if not RISCV_TESTS.exists():
        pytest.skip("riscv-tests not fetched")
    assert makefile_list(EXTS[ext]) == upstream_list(ext)


def run_test(core, test):
    elf = BUILD / "rvtests" / f"{test}.elf"
    r = run_vsim(core, elf, "--max-cycles", "2000000")
    assert r.returncode == 0, f"{test} on {core}: exit {r.returncode}\n{r.stderr[-3000:]}"


@pytest.mark.parametrize("core", ALL_CORES)
@pytest.mark.parametrize("test", INT_TESTS)
def test_riscv_test(core, test):
    run_test(core, test)


@pytest.mark.parametrize("test", FP_TESTS)
def test_riscv_fp_test(test):
    run_test(FP_CORE, test)


@pytest.mark.parametrize("core", ["single", "pipe"])
def test_fp_is_illegal_without_fpu(core):
    """On the RV32IM cores every F/D instruction traps: the F test's first FP
    CSR access raises an illegal-instruction exception, which the test
    environment reports as a failure (and the lockstep comparison agrees)."""
    r = run_vsim(core, BUILD / "rvtests" / "rv32uf-fadd.elf", "--max-cycles", "2000000")
    assert r.returncode == 1, r.stderr[-2000:]
