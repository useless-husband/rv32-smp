## Correct protocol: exhaustive search

| caches | addresses | states (after symmetry reduction) | transitions | depth | time | result |
|---:|---:|---:|---:|---:|---:|---|
| 2 | 1 | 324 | 1152 | 22 | 0.00 s | all properties hold |
| 2 | 2 | 16155 | 66436 | 30 | 0.01 s | all properties hold |
| 3 | 1 | 3343 | 14372 | 34 | 0.00 s | all properties hold |
| 3 | 2 | 500087 | 2621821 | 53 | 1.64 s | all properties hold |

## Bug variants: shortest counterexamples

| variant | first internal invariant broken | steps | first visible effect | steps | caches x addresses |
|---|---|---:|---|---:|---|
| lost upgrade | data-value invariant violated | 15 | a read returned a stale value | 16 | 2 x 1 |
| write-back buffer not snooped | a read returned a stale value | 9 | a read returned a stale value | 9 | 2 x 2 |
| LR reservation survives a remote write | SC succeeded after a remote write | 13 | SC succeeded after a remote write | 13 | 2 x 1 |
| E despite a shared copy | single-writer/multiple-reader violated | 8 | a read returned a stale value | 10 | 2 x 1 |
| LR lock-out without timeout | a request can never finish (no progress) | 7 | a request can never finish (no progress) | 7 | 2 x 1 |
