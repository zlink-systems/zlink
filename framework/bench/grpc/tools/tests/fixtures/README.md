# Fixtures

Every fixture run is laid out as a runner writes it (README section 11):
`<root>/run<N>/<implementation>-<pattern>-<payload>/results.json`, a
`with-grpc-cell-v1` document.

`gated2/{c,dotnet}/run{1,2,3}` is the Phase 0 ROUTER measurement, converted from
the printed reports to cell JSON. The full run directories (server logs, pid
files) stay in the job worktree; nothing here is a measurement archive, it is the
input that pins the aggregator's acceptance criterion.

The expected output is transcribed into `tests/test_acceptance_gated2.py`, which
compares the two cell by cell.

`s2s/paired/run{1,2,3}` carries a source cell with embedded `target_stats`, and
`missing-target/run1` carries a source without them.

`compare-results/` holds the before/after runs for `compare-results.py`.
