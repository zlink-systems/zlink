2026-09-05T17:39:07+09:00 START: preserving detached worktree and existing tests; inspecting correlation admission and writable activation.
2026-09-05T17:41:23+09:00 DESIGN: preserve correlation refusal via observer; queue-owned token predicates use rejecting pair release edge; existing DEALER candidate fallback retained.
2026-09-05T17:45:51+09:00 IMPLEMENTED: correlation refusal metadata, pipe return epoch, existing mailbox callback and queue token filtering; strengthened 64KiB zero-before/one-after assertions; build -j3 running.
2026-09-05T17:50:13+09:00 FOCUSED PASS: first 30-sample credit regression and existing writable contract/queue tests; adding timeout, disconnect, explicit removal, close and same-socket pair-isolation coverage.
2026-09-05T17:53:07+09:00 REGRESSION PASS: 5/5 runs, 11 cases/run (150 size/transport samples plus lifecycle/pair-isolation cases); integration -j2 running. Explicit-removal request expectation corrected to spec README:1147 NOT_FOUND (token remains TERMINAL+ENOENT).
2026-09-05T17:54:54+09:00 REVIEW: retained-pipe token ownership released on WRITABLE/terminal/close; registration recheck compares pipe return epochs. Preserved metadata OOM errno for final rebuild; integration currently passing.
2026-09-05T17:58:09+09:00 INTEGRATION GREEN: 126/126 in 217.55s (-j2), including backpressure/router/single-lane suites. Final -j3 rebuild incorporates observer metadata allocation-error preservation; final five runs and full CTest follow.
2026-09-05T17:59:28+09:00 FINAL BUILD: -j3 ongoing, no compiler warnings/errors observed; five-run sample distribution remains BP=1/WRITABLE-before-reply=0 for both 64KiB transports and all retries admitted after release.
2026-09-05T18:01:31+09:00 FINAL REGRESSION GREEN: rebuilt credit binary 5/5, 11 cases/run; final full build nearing completion, then ctest -j2 -E hotpath_gate.
2026-09-05T18:04:37+09:00 FULL GATE: ctest -j2 -E hotpath_gate running with no failures so far; summary drafted with exact causes, pair/candidate review, final regression distribution and hot-path handoff.
2026-09-05T18:06:39+09:00 COMPLETE: final regression 5/5; integration 126/126; full CTest 176/176 (249.08s, hotpath_gate excluded); git diff --check clean; summary finalized; no remaining failures.
EXIT:0
