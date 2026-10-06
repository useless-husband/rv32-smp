# Mutation test of the coherence hardware

10 / 10 mutants killed.

| mutant | the bug it introduces | result |
|---|---|---|
| `store_no_dirty` | a store hit does not mark the line dirty, so the write is lost on eviction | killed by rv32ua-lrsc@vsmp2:h0 |
| `snoop_no_inval` | a snooped BusRdX/BusUpgr does not invalidate the local copy (two writers) | killed by atomics@vsmp2 |
| `read_takes_e` | a read miss installs E in way 0 even when another cache shares the line | killed by atomics@vsmp2 |
| `sc_always` | SC succeeds without a matching reservation (atomicity broken) | killed by rv32ua-lrsc@vsmp2:h0 |
| `resv_not_cleared` | a remote store does not clear the LR reservation | killed by atomics@vsmp4 |
| `upgrade_not_converted` | a pending BusUpgr losing its copy is not turned into a BusRdX (lost upgrade) | killed by stress1@vsmp2 |
| `wb_not_snooped` | the write-back buffer does not answer snoops (a request reads stale memory) | killed by wb_race@vsmp2 |
| `bus_no_shared` | the bus never reports a line as shared, so readers take E (silent upgrade later) | killed by atomics@vsmp2 |
| `bus_no_snoop` | the bus snoops no other cache on a coherent request (no invalidations happen) | killed by rv32ua-lrsc@vsmp2:h0 |
| `atomic_not_load_use` | an AMO/LR result is not interlocked against a dependent instruction | killed by rv32ua-lrsc@vsmp2:h0 |
