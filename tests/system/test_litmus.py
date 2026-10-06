"""Litmus tests on the RTL, checked against the reference model's verdicts in
the fetched suite (data/litmus/model-results/flat.logs, the operational RVWMO
model).  Every outcome the core produces must be allowed by the reference
model; because the core is sequentially consistent it never shows a relaxed
outcome, which is itself reported."""

import json
import re
import subprocess

import pytest

from conftest import BUILD, REPO, vsmp

LOGS = REPO / "data" / "litmus" / "model-results" / "flat.logs"


def verdicts():
    if not LOGS.exists():
        pytest.skip("litmus reference results not fetched (make litmus-fetch)")
    v = {}
    for line in LOGS.read_text(errors="ignore").splitlines():
        m = re.match(r"Observation (\S+) (Always|Sometimes|Never) ", line)
        if m:
            v[m.group(1)] = m.group(2)
    return v


def test_every_observed_outcome_is_allowed():
    ref = verdicts()
    # not --quiet: the program prints its JSON result through the console
    cmd = [str(vsmp(4)), "--seed", "7", "--stall-pct", "20", "--jitter", "8", "--max-cycles", "80000000",
           str(BUILD / "sw" / "litmus.elf")]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, cwd=REPO)
    assert r.returncode == 0, r.stderr[-2000:]
    data = json.loads(r.stdout[r.stdout.index("{"):])
    checked = 0
    for t in data["tests"]:
        if not t["ran"]:
            continue
        v = ref.get(t["name"])
        assert v is not None, f"no reference verdict for {t['name']}"
        if v == "Never":
            assert t["relaxed"] == 0, (
                f"{t['name']}: the reference model forbids the relaxed outcome, "
                f"but the core showed it {t['relaxed']} times")
        checked += 1
    assert checked >= 8, f"only {checked} litmus tests ran"
