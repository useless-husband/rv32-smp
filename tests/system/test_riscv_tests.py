"""Every riscv-tests ISA test (rv32ui + rv32um + rv32ua) runs on EVERY hart of
the 1-, 2- and 4-hart builds, in lockstep with that hart's golden model and
with the protocol-model agreement check turned on.  A multicore must still be
a correct uniprocessor on each core."""

import subprocess

import pytest

from conftest import BUILD, HART_COUNTS, REPO, run, vsmp

# rv32ua minus the Zacas instructions (amocas_*), which this core does not implement
RV32UA = ["amoadd_w", "amoand_w", "amomax_w", "amomaxu_w", "amomin_w", "amominu_w", "amoor_w",
          "amoswap_w", "amoxor_w", "lrsc"]


def all_tests():
    d = BUILD / "rvtests"
    if not d.exists():
        pytest.skip("riscv-tests not built (run make rvtests)")
    return sorted(p.stem for p in d.glob("*.elf"))


@pytest.mark.parametrize("n", HART_COUNTS)
def test_every_hart_passes_isa_suite(n):
    sim = vsmp(n)
    tests = all_tests()
    assert tests, "no riscv-tests ELFs"
    for k in range(n):
        for t in tests:
            r = run(sim, BUILD / "rvtests" / f"{t}.elf", "--model", "--run-hart", str(k))
            assert r.returncode == 0, f"{t} failed on hart {k}/{n}: exit {r.returncode}\n{r.stderr[-2000:]}"


def test_atomics_suite_present():
    for t in RV32UA:
        assert (BUILD / "rvtests" / f"rv32ua-{t}.elf").exists(), f"rv32ua-{t} not built"
