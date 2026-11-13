Model gpt-6-luna, effort high: a same-session A/B measurement with existing bench scripts; no design or code change.

# Task: Java binding cost of PR #4073 on the request cells — main vs PR, same session (#4060)

## Situation
PR #4073 (branch fix/4060-java-binding-submit-close, worktree /root/worktree/zlink-4060-binding-close, HEAD b42f978451 = the commit to measure; it also moves the reservation into SocketCore for PUB publish, see /root/jobs/fx/cx/v21/s033/bpub/report.md) adds a reservation to the Java binding submit path: synchronous send and reply gain one ownerLock entry and two atomic read-modify-writes per call; other paths keep main's lock count. The binding pair's throughput is the denominator of Framework÷binding, so the PM requires the binding cost before merge.
Bench: framework/bench/grpc/java/run_local.sh (implementation `zlink-java` = raw binding pair). Find how it picks the Java binding jar (settings.gradle.kts includeBuild / ZLINK_JAVA_BINDINGS_SOURCE dependency substitution, as used in /root/jobs/fx/cx/v21/s033/bclose/report.md for the Framework tests) and make sure each side really loads its own binding build (record the jar path and sha256 loaded by each run).
Main side: a clean scratch worktree of origin/main 6b24e01426 (the PR's current merge base) (create it with `git -C /root/zlink worktree add --detach /root/worktree/zlink-jbind-main <sha>`; remove it at the end with `git -C /root/zlink worktree remove`). PR side: the PR worktree at HEAD.

## Do
0. The PR also adds one ownerLock entry per publish (PUB/XPUB). Check whether the bench has any publish pattern; if not, say so in the report and use send-saturation as the stand-in (the PM agreed).
1. Build the bench and binding for each side (Java builds via `/root/jobs/fx/cx/measure-shared.sh <cmd>`).
2. Measure `zlink-java`, patterns request-serial, request-backpressure and send-saturation, payload 1024, RUNS=5 per side, alternating main/PR order per run, condition B (source CPUs 0-7, target CPUs 8-15; check affinity; use the repository's pinning already in run_local.sh/runner_common.sh or the same taskset method the other jobs used — state which). Only inside `flock -o /root/jobs/fx/cx/measure.lock <cmd>`, held only while measuring.
3. Also, at one common paced rate per pattern (pick a rate both sides reach), total CPU per request (source+target, user+kernel), 3 runs each, if the bench supports pacing; otherwise say so.
4. Report per cell: main and PR medians (with all 5 values), difference in % and µs/request, and whether the difference is outside the run-to-run spread.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except bclose/, bpub/; write only in jbind/, the scratch main worktree, and build outputs.
- Do not edit tracked files in either worktree (bench pin changes, if needed, go in a scratch copy and are listed). No git operations except worktree add/remove as above, diff, status, log, rev-parse. No GitHub operations; no spec edits. Do not read core/include. Do not run scripts/dev/internal-docs.sh.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- 리팩토링 점검: not applicable beyond noting anything odd in the bench setup.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/jbind/report.md (Korean, plain sentences): (1) setup, jar paths and hashes per side, pinning; (2) table per cell main vs PR with 5 values, medians, % and µs/request; (3) paced CPU if done; (4) verdict per cell: no measurable change / slower by X.
