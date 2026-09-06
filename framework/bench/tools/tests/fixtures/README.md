# Fixtures

`gated2/` is the Phase 0 ROUTER measurement, reduced to the parts the aggregator
reads: the effective-options header, the `RESULT` lines, and the `[bench]`
diagnostic lines from each run's stdout. The full run directories (server logs,
`results.json`, pid files) stay in the job worktree; nothing here is a
measurement archive, it is the input that pins the aggregator's acceptance
criterion.

`doc/plan/fw-bench-worklog/bench-dotnet-summary.ko.md` is the expected output.
`tests/test_acceptance_gated2.py` compares the two cell by cell.
