Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 C++ L0 — measure the receive-parity build. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/l3868, branch exp/3868-cpp-ladder (the name is historical; this is #4350), commit 73285401ca. Never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (applies here) and /root/jobs/fx/cx/v21/s033/l3868-l0e/report.md (the previous job: source change, parity table, prepared tools; it ran out of time before building).

## Build
kairos is building it now with l3868-l0e/tools/configure-build-inner.sh into build-l3868 (Release, bindings source ON, Core 1.25.0 prefix). Do NOT start your own build. Poll every 60 s for /root/jobs/fx/cx/v21/s033/l3868-l0f/BUILD_DONE; it holds `rc=<n>`. If rc is not 0, read l3868-l0e/raw/build.log, fix the source, commit, and rebuild only through `/root/jobs/fx/cx/cpp-build-slot.sh <your copy of configure-build-inner.sh>` with JOB pointing at l3868-l0f; then tell kairos nothing — just continue.
Before measuring: binaries newer than every changed source file (git show --stat 73285401ca), and run the built unit tests (bench_cpp_raw_wire_test etc.) once.

## Measure
1. Copy l3868-l0e/tools (run-group.sh, measure.sh, preflight-measure.sh) to l3868-l0f/tools and change only JOB/raw paths. Progress line before each measurement as common-rules says.
2. L0 vs binding, 6 cells × 3 runs in one lock session; G5 failures and request-serial 4K → 5 runs. G5 spread = largest |run − median| ÷ median.
3. Cross test send-saturation 1K × 3 runs: raw/raw, L0 server/raw client, L0/L0.
4. Any cell below 0.95: one paired perf profile (raw and L0 target, same cell) and the frame difference with file:line. Do not change product code.

## Output
/root/jobs/fx/cx/v21/s033/l3868-l0f/report.md (Korean, short tables, every number cites its raw file): 6-cell table (medians, ratio, spread, judgement), cross table, profile difference for failing cells. Time cap 4 h from BUILD_DONE.
