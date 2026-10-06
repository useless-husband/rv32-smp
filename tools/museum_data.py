#!/usr/bin/env python3
"""Collect the bug-museum data the HTML report embeds: for each of the five
bugs, the model checker's shortest counterexample (build/museum/mc_bugK.json,
written by `make mc`) and a short RTL reproduction (the coherence log of the
directed program on the buggy build, up to the point the checker/model/
deadlock flags it).  Writes docs/museum.json."""
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "build"

BUGS = [
    (1, "lost_upgrade", "Lost upgrade race",
     "Two caches hold a line Shared and both write it. Each asks the bus to upgrade "
     "(BusUpgr, no data). The bus serves one; the other loses its Shared copy to that "
     "invalidation while it waits. The correct cache turns its upgrade into a BusRdX and "
     "re-reads the winner's data; the broken one upgrades a copy it no longer has, so the "
     "winner's write is lost."),
    (2, "wb_race", "Request crossing a write-back",
     "A dirty line is evicted into the one-entry write-back buffer and the buffer is drained "
     "to memory only after the new line is filled. If another cache asks for that line while "
     "it still sits in the buffer, the buffer holds the only current copy and must answer. The "
     "broken cache ignores its buffer, so the requester reads the stale value from memory."),
    (3, "lrsc_clear", "Reservation not cleared by a remote store",
     "A hart does LR on a line, then another hart writes it, then the first hart does SC. The "
     "SC must fail: the value changed under it. The broken cache does not clear the reservation "
     "when a remote store invalidates the line, so the SC succeeds and silently overwrites the "
     "other hart's store - the atomicity LR/SC promises is gone."),
    (4, "e_shared", "Exclusive granted despite a sharer",
     "One cache reads a line (Exclusive, since nobody else has it). A second cache reads the same "
     "line: it must get Shared, and the first must drop to Shared too. The broken bus never reports "
     "the line as shared, so the second cache takes Exclusive and its next store is a silent "
     "E->M upgrade with no invalidation; the first cache keeps reading its stale copy."),
    (5, "lockout", "LR lock-out with no timeout",
     "To give an LR/SC loop a chance to make progress, snoops to a freshly reserved line are "
     "deferred for a short window. The window must always close. The broken cache defers forever: "
     "a hart that holds a reservation and waits (on a flag another hart will set) blocks that other "
     "hart's store indefinitely - a deadlock the bus cannot break."),
]


def model_cex(bug):
    p = BUILD / "museum" / f"mc_bug{bug}.json"
    if not p.exists():
        return None
    return json.loads(p.read_text())


def rtl_repro(prog, bug):
    sim = BUILD / f"vsmp2-bug{bug}"
    if not sim.exists():
        return None
    log = BUILD / f"museum_{prog}.jsonl"
    budget = "150000" if bug == 5 else "4000000"
    r = subprocess.run([str(sim), "--quiet", "--model", "--coh-log", str(log), "--max-cycles", budget,
                        str(BUILD / "sw" / f"{prog}.elf")], capture_output=True, text=True, cwd=REPO)
    events = []
    if log.exists():
        for line in log.read_text().splitlines():
            try:
                events.append(json.loads(line))
            except ValueError:
                pass
    # keep the tail (the interesting part is where it goes wrong)
    tail = events[-14:]
    verdict = r.stderr.strip().splitlines()
    reason = next((l for l in verdict if "FAILED" in l or "DISAGREES" in l or "no exit" in l), "")
    return {"events": tail, "reason": reason, "exit": r.returncode}


def main():
    out = {"bugs": []}
    for bug, prog, title, expl in BUGS:
        out["bugs"].append({
            "id": bug, "prog": prog, "title": title, "explanation": expl,
            "model": model_cex(bug), "rtl": rtl_repro(prog, bug),
        })
    (REPO / "docs" / "museum.json").write_text(json.dumps(out, indent=1))
    print(f"wrote docs/museum.json ({(REPO / 'docs' / 'museum.json').stat().st_size} bytes)")


if __name__ == "__main__":
    main()
