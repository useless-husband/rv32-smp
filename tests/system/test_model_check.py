"""The explicit-state model checker proves the invariants for small
configurations, and finds a counterexample for every bug variant."""

import subprocess

import pytest

from conftest import BUILD, REPO


def mc():
    p = BUILD / "mc"
    if not p.exists():
        pytest.skip("model checker not built (run make mc)")
    return p


@pytest.mark.parametrize("n,a", [(2, 1), (2, 2), (3, 1)])
def test_correct_protocol_holds(n, a):
    r = subprocess.run([str(mc()), "--bug", "0", "--n", str(n), "--a", str(a)],
                       capture_output=True, text=True, cwd=REPO)
    assert r.returncode == 0 and "all properties hold" in r.stdout, r.stdout


@pytest.mark.parametrize("bug", [1, 2, 3, 4, 5])
def test_bug_has_counterexample(bug):
    r = subprocess.run([str(mc()), "--bug", str(bug), "--n", "2", "--a", "2", "--observable"],
                       capture_output=True, text=True, cwd=REPO)
    assert r.returncode == 1, f"bug {bug} produced no counterexample:\n{r.stdout}"
