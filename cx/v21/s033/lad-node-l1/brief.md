Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 Node L0 confirm + #4351 Node L1 (permit). EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, HEAD dba747fe85 (L0 aligned to the raw server shape: two routers, no Framework/Nest/codec requires on the L0 path). Commit L1 locally on top; never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (FIRST OUTPUT and SESSION SHAPE rules), lad-node-l0e/report.md, lad-node-l0f/report.md, lad-node/report.md (L0 feature table: the permit row with file:line), issues-4351-4357.md section #4351. For reference: Java L1 used ZLinkApplicationJobQueue.acquireBlocking(REMOTE) before receive and returned the permit just before the handler's first instruction (lad-java-l1/report.md); .NET L1 is b6f58168a6 in /root/worktree/zlink-v21-s033/lad-dotnet (ZLinkServer/Program.cs:52-63,162-202).

## Known
All Node numbers before 12:08 UTC 11-13 are provisional (the quiet guard did not pause Java/Node builds and tests then). lad-node-l0f variants (11:35–11:59 UTC): binding 8,144.8, L0 a+b 7,983.2 (0.980) at request-serial 4K.

## Module-load cost (PM 11-13 12:30)
L0 now avoids requiring Framework/Nest/codec, but that cost (lad-node-l0f profile: recv +6.1%p, (program) +7.4%p, GC +1.2%p, idle −25.7%p; target memory 137 MB vs 78 MB) does not vanish in the product — it returns when the modules come back. The L1 permit needs the Framework module, so L1 brings back at least the Framework require. Measure it separately so the permit's own cost is not mixed with module loading:
- add an env switch "L0R" = L0 + the same top-level requires L1 will load (Framework core module only, NOT Nest or codec unless L1 really needs them), no permit;
- step losses: L0R vs L0 (= module-load cost), L1 vs L0R (= permit cost), L1 vs L0 (= the ladder step). Report all three; the ≤ 7% verdict is L1 vs L0.
- If L0R vs L0 is above 2%, take one paired --cpu-prof + heap usage (process.memoryUsage at end of run) for L0 and L0R at request-serial 4K and say which frames/GC change with file:line.

## Do
1. FIRST OUTPUT: lad-node-l1/call-shape.md for L0 (dba747fe85) and L1 vs binding. Only the permit may differ at L1.
2. Restore ONLY the permit on top of L0 using the real Node Framework application job queue (the class the L0 report lists under permit), acquired before each receive and returned just before the handler's first instruction, behind an env switch so L0 and L1 run from the same build. Keep the raw server's setImmediate pump and counters. Commit; record file:line.
3. Build (framework/bench/grpc/node/build.sh), verify dist newer than sources.
4. One lock session: binding, L0, L0R, L1 — 6 cells × 3 runs; request-serial 4K and any G5-failing cell → 5 runs (for all three). G5 spread = largest |run − median| ÷ median; still failing after 5 runs = 판정 불가.
5. Judge L0 ≥ 0.95 × binding; L1 step loss = 1 − (L1÷binding)/(L0÷binding) from this session, pass ≤ 7%. Over 7%: paired --cpu-prof, cost file:line, binding-cost proposal; do not implement.

## Output
lad-node-l1/report.md (Korean, short tables, numbers cite raw files): call-shape summary, L0 table, L1 table (step loss, G5, verdict), restored code file:line + commit. Time cap 4 h.
