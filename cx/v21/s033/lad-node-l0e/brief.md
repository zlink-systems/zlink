Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: fix and re-measure #4350 ladder L0 — Node. Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder.
Read first: /root/jobs/fx/cx/v21/s033/issue-4350.md; the previous report /root/jobs/fx/cx/v21/s033/lad-node/report.md; /root/jobs/fx/cx/v21/s033/common-rules.md (binding).

## Do
1. Receive parity: the previous report found request-serial 1K/4K at 0.937/0.938 because the L0 worker waits with `poller.wait()` per request and does two `Atomics.add` per message (l0-worker.js:18,21,73-74), while the raw server uses the `setImmediate(pump)` loop (zlink-raw-server/main.js:132-142) and plain counters (shared/bench-server-metrics.js:33-40). Make the L0 server use the raw server's receive loop and the same metrics counters. If the L0 server must stay on a worker thread for structural reasons, say why with file:line; otherwise run it like the raw server. Commit.
2. Re-measure all 6 cells, L0 and binding in the same session, 3 runs. Then repeat the L0 cross test at send-saturation 1K with the L0 client (as before).
3. Judge L0 ≥ 0.95 × binding per cell; if a cell is under, attribute the cost with file:line and a profile.
Output dir /root/jobs/fx/cx/v21/s033/lad-node-l0e (progress.md, report.md, raw/, tools copied from ../lad-node/tools). Keep the old raw. Time cap 4 h.
