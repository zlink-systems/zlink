Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 C++ L0 confirm + #4351 C++ L1 (permit). EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/l3868, branch exp/3868-cpp-ladder (historical name; this is #4350/#4351), HEAD 73285401ca (L0 with receive parity). Commit L1 locally on top; never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (FIRST OUTPUT, SESSION SHAPE, C++ build rules), l3868-l0f/report.md, l3868-l0e/report.md, l3868/report.md (L0 feature table: permit row with file:line), issues-4351-4357.md section #4351. Reference: Java L1 ZLinkApplicationJobQueue.acquireBlocking(REMOTE) before receive, permit returned just before the handler's first instruction; .NET L1 b6f58168a6 (/root/worktree/zlink-v21-s033/lad-dotnet, ZLinkServer/Program.cs:52-63,162-202).

## Known (l3868-l0f, measured 12:05–13:45 UTC, kairos checked raw)
- Pass: request-serial 1K 0.986, request-backpressure 1K 1.019, request-backpressure 4K 1.016.
- send-saturation 1K 0.934: invalid — 2 of 3 runs started before the quiet guard was tightened (12:05:35 UTC).
- send-saturation 4K (binding run 355,151 vs ~238–258k) and request-serial 4K (L0 run1 7,725 vs ~11,650): G5 failed by a single run each → 판정 불가.
- Cross send 1K: L0/L0 ÷ raw/raw 1.023, all pass.

## Do
1. FIRST OUTPUT: l3868-l1/call-shape.md for L0 and L1 vs binding (only the permit may differ at L1).
2. Restore ONLY the permit on top of L0 using the real C++ Framework application job queue (the class/file the L0 feature table lists under permit), acquired before each receive, returned just before the handler's first instruction; switch L0/L1 at run time (env or flag) so both run from one build. Keep L0 receive/reply parity. Commit; record file:line.
3. Build only via /root/jobs/fx/cx/cpp-build-slot.sh with a copy of l3868-l0e/tools/configure-build-inner.sh (JOB → l3868-l1; Release, bindings source ON, Core 1.25.0 prefix, -j3, build dir build-l3868). Builds can wait long at the measure gate; if yours waits more than 30 min, write a line "GATE-WAIT <minutes>" to progress.md and keep waiting (kairos will let it through). Verify binaries newer than changed sources; run the built unit tests once.
4. One lock session: binding, L0, L1 — 6 cells × 3 runs; request-serial 4K, send-saturation 4K and any G5-failing cell → 5 runs for all three. G5 spread = largest |run − median| ÷ median; still failing after 5 runs = 판정 불가 (list the outlier runs with their start times).
5. Judge L0 ≥ 0.95 × binding; L1 step loss = 1 − (L1÷binding)/(L0÷binding) from this session, pass ≤ 7%. Over 7%: paired perf, cost file:line, binding-cost proposal; do not implement.
6. Remove build-l3868 at the end (keep raw).

## Output
l3868-l1/report.md (Korean, short tables, numbers cite raw files): call-shape summary, L0 table, L1 table, restored code file:line + commit. Time cap 5 h.
