"""The bug museum: each protocol bug, built into its own RTL variant, is caught
end to end by a directed two-hart program; the same program passes on the
correct build.  A bug that no checker catches is not a demonstration."""

import pytest

from conftest import BUILD, run, vsmp

CASES = [
    (1, "lost_upgrade"),
    (2, "wb_race"),
    (3, "lrsc_clear"),
    (4, "e_shared"),
    (5, "lockout"),
]


@pytest.mark.parametrize("bug,prog", CASES)
def test_correct_build_passes(bug, prog):
    r = run(vsmp(2), BUILD / "sw" / f"{prog}.elf", "--model", "--max-cycles", "4000000")
    assert r.returncode == 0, f"{prog} should pass on the correct build:\n{r.stderr[-2000:]}"


@pytest.mark.parametrize("bug,prog", CASES)
def test_buggy_build_is_caught(bug, prog):
    # the deadlock bug (5) hangs: a short cycle budget and a nonzero exit is the catch
    budget = "200000" if bug == 5 else "4000000"
    r = run(vsmp(2, bug=bug), BUILD / "sw" / f"{prog}.elf", "--model", "--max-cycles", budget)
    assert r.returncode != 0, f"bug {bug} ({prog}) was NOT caught on the buggy build"
