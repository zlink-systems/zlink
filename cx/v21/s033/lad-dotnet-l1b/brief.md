Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4351 .NET ladder L1 (permit) — measure; L0 request-serial re-measure. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder, HEAD b6f58168a6 (L1 permit, parent a542ee2384 = L0 with receive parity). Never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (READ THE "FIRST OUTPUT" RULE) and /root/jobs/fx/cx/v21/s033/lad-dotnet-l1/report.md (previous job: fresh L0 table, L1 code; it hit the time cap before building L1).

## Fresh L0 (from lad-dotnet-l1, kairos verified from raw)
send 1K 1.010 · send 4K 1.016 · bp 1K 0.997 · bp 4K 0.966 — pass.
request-serial 1K (binding G5 17.9%) and request-serial 4K (L0 G5 39.1%; one L0 run at 5,802.8) — 판정 불가 after 5 runs.

## Do
1. FIRST OUTPUT: /root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/call-shape.md for L0 (a542ee2384) and L1 (b6f58168a6) vs binding, per common-rules. The only L1 difference allowed is the permit (acquire before receive, return before the handler's first instruction). Do not measure until the table is clean.
2. Fresh Release build of b6f58168a6 (build && DLLs newer than sources && measure). Copy lad-dotnet-l1/tools, change only JOB/raw paths. Progress lines per common-rules.
3. One lock session: binding, L0 and L1 for all 6 cells × 3 runs; request-serial 1K/4K × 5 runs for all three. If request-serial still fails G5, record each run's start time and the target/source CPU% from results.json, and check the run log for anything else running (lock-windows); report it as 판정 불가 with that evidence — no pass from medians.
4. Step loss L1 = 1 − (L1÷binding)/(L0÷binding) with this session's L0. Pass ≤ 7%. Over 7%: paired profile, cost with file:line, binding-cost proposal; do not implement.

## Output
/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/report.md (Korean, short tables, numbers cite raw files): call-shape summary, L0 and L1 tables (medians, ratio, spread, step loss, verdict). Time cap 4 h.
