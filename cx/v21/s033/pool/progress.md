# Progress

- Branch: `perf/1639-pool-exp` at base `168c679590551bf9e102bc41bbbfebbdd7b48de1`; initial worktree was clean.
- Copied the psched8 harness into this job folder. `configure-branch.sh` now points to this worktree and `.pool-build`.
- Updated only `framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp`: a valid positive `ZLINK_PERF_COMPLETION_POOL` overrides the default count, and each worker is named `zl-cmpl-<n>`.
- Configure is running through `cpp-build-slot.sh`; no measurement has started.
- Waiting for `/root/jobs/fx/cx/v21/s033/bstage/report.md` before measurement.
- Local commit created: `17c5a80757` (`perf(cpp): make completion pool size configurable`).
- Configure is currently waiting at the build-slot measurement gate while bstage is active; available memory is sufficient. The bstage report remains absent.
- Copied `runner-common.sh` and the `bench/` support files too, and redirected the local runner to those copies.
- Read bstage report: launch difference is inconclusive, so the measurement will use the psched8 Python supervisor. TID coverage for target app-worker attribution will be recorded alongside the summed value.
- Configure completed; Release build is running through `cpp-build-slot.sh` (3 of 602 initial Ninja actions completed at kickoff; dispatcher translation unit compiled successfully).
- `run-experiment.sh` is prepared: each round rotates the variant order; each benchmark invocation holds the measurement lock; Framework throughput, PM Spot, and 22k captures use separate output paths.
- Target build completed for `bench_cpp_client`, `bench_cpp_framework_server`, and `bench_cpp_pm_server`. Each executable is newer than `provider_task_context.cpp`; `strings` finds `ZLINK_PERF_COMPLETION_POOL` in all three. `bash -n` passed for the copied runner scripts.
- Base build binaries are present. Starting the 3-round experiment using the psched8 supervisor, with pool order base/pool8/pool4/pool2, then pool8/pool4/pool2/base, then pool4/pool2/base/pool8.
- All measurements completed: 24 throughput cells, 12 PM Spot captures, and 12 paced 22k captures. Raw JSON/latency/thread/perf/log files were read and the 22k analyzer generated per-round thread summaries.
- The captured source pool counts are 2/4/8 named TIDs as requested. Base has no completion names; the fallback residual includes 35 unnamed source TIDs after excluding I/O, named roles, and process leader. Target app-worker snapshot coverage ranges from 1/3 to 2/3; the report will expose coverage with the summed value.
- Preparing the final report from the raw files. No benchmark processes remain.
- Final report generated from validated raw captures at `report.md`; all 61 report links resolve. Throughput/PM and 22k values are recorded with base's same-session 3-round bands.
- Local worktree remains on `perf/1639-pool-exp` with commit `17c5a80757`; only `.pool-build/` is untracked. It is retained because the copied experiment runner and conditions file reference it. No benchmark processes remain; no tests ran.
