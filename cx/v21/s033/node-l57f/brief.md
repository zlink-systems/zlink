Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Node ladder L5~L7 — the bench run is invalid (target received 0; source stats endpoint never ready). Find why and fix the BENCH wiring if it is a bench fault; if it is a Framework fault, report it with a reproducer and stop.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, tip 7d1afa98a9 (L5~L7: zlink-framework-server/main.js, client/framework-transport.js, bench-handlers.js). No other job uses this worktree now. Commit locally only; never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (incl. LOAD TESTS DURING OTHER HOLDS), ladder-steps.md, lad-node-impl4c/report.md, lad-node-s1/report.md ("L5~L7 미완료 cell" section and its raw links).

## Evidence (lad-node-s1)
- L5~L7 send-saturation 1K warm: submitted = completed = 75,055 but target received 0.
- L5~L7 request-serial 1K warm: the source stats endpoint was not ready within 30 s, twice.
The L5~L7 path goes through the public Framework route with the default JSON serializer (protobuf registration off). Check, do not assume: handler registration name/route vs the message the client sends (sends that find no handler are dropped silently and still complete at the source), metrics wiring in the typed handlers (bench-handlers.js), the client's L5_7 startup path and its stats server, env switches that one side sets and the other does not (ZLINK_LADDER_L5_7_SERVER / _CLIENT).

## Do
1. Reproduce with a short smoke run of L5~L7 send-saturation 1K and request-serial 1K, each run inside your own exclusive hold: flock -x /root/jobs/fx/cx/measure.lock, at most 5 minutes per hold, pin source 0-5 / target 8-13. Enable the Framework message-flow trace if available to see where sends go.
2. Report in /root/jobs/fx/cx/v21/s033/node-l57f/report.md FIRST: cause with file:line, evidence (raw under node-l57f/raw). If the cause is in Framework production code (e.g. a send that completes at the source but is never delivered or never dispatched, without the documented error), STOP there: write the exact reproduce command and the production file:line; do not work around it in the bench.
3. If it is a bench fault: fix it in one commit "bench(node): fix ladder L5_7 <what> (#4355)", npm test, then a smoke run of all 6 cells (one short run each, own hold ≤ 10 min) showing errors 0 and submitted = completed = received. No throughput judgement.
Time cap 2 h.
