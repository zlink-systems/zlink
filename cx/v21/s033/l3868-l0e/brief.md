Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: finish #4350 ladder L0 — C++. Worktree /root/worktree/zlink-v21-s033/l3868, branch exp/3868-cpp-ladder (HEAD 666edd8dc2 has the corrected L0).
Read first: /root/jobs/fx/cx/v21/s033/issue-4350.md; the previous report /root/jobs/fx/cx/v21/s033/l3868/report.md and raw/cost-findings.md; /root/jobs/fx/cx/v21/s033/common-rules.md (binding).

## Do
1. Receive parity: the previous run found the L0 target waits with the owner poller + DONTWAIT drain while the raw C++ server blocks in recv directly. Make the L0 receive loop the raw server's receive loop (cite both file:line). Also check the source-side "native send buffer copy" and "temporary Message lifetime" items in cost-findings.md: if the L0 source does any copy or allocation the raw client does not, remove it so L0 matches the raw client. Commit.
2. Measure all 6 cells fresh (send-saturation, request-serial, request-backpressure × 1K, 4K), L0 and binding in the same session, 3 runs; send-saturation 4K gets 5 runs (G5 failed last time). Then the L0 cross test at send-saturation 1K (raw/raw, Framework L0 server + raw client, Framework L0 server + Framework L0 client).
3. Judge L0 ≥ 0.95 × binding per cell. If a cell is under, attribute the remaining cost (file:line, with a perf profile or allocation/lock count).
Output dir /root/jobs/fx/cx/v21/s033/l3868-l0e (progress.md, report.md, raw/, tools/run-group.sh copied from ../l3868/tools with only JOB/raw paths changed). Keep the old l3868 raw. Time cap 5 h.
