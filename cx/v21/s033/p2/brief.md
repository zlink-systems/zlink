Model gpt-6.1-sol, effort high: changing mailbox ownership and worker take order on the C++ Framework's concurrent receive path (FIFO, last-take/enqueue, shutdown, Spot/Actor gates) needs the stronger model.

# Task: step 2 of the approved C++ path design (#1639) — remove Node/Channel prefix claim, settlement and host batch repacking; hand the queue head to the worker directly

Earlier job briefs are history, not instructions. The design is /root/jobs/fx/cx/v21/s033/cpath/report.md, PM-approved on 2026-11-10 for steps 1–7. Read §3 (the mailbox row, line 109), §4 intro (lines 119, 125), §4.2 (R3 at line 164), §6b (lines 275–288, especially the cdrain and csendb rows), §7 row 2 and the count rules, and §8 (lines 338, 340, 356) before you start. This job implements row 2 of §7 only. Step 1 (canonical receive record, PR #4155) is already in this branch; read /root/jobs/fx/cx/v21/s033/p1/report.md for what it changed. The design's file:line values are at 532cd83b67; find the current lines yourself.

## Scope (design §7 row 2)
- Node/Channel: delete the prefix claim (`make_shared<service_mailbox_claim_t>`, `records`/`record_bytes` vectors, settled flags, `remaining`) and the host batch repacking (`public_host_runtime.cpp` claim→batch→record→route received, `mesh_record_dispatcher.cpp`). A worker takes one queue head (the canonical record from step 1) and owns it until the handler terminal: payload, context and reply token stay in the record.
- Remove the rule that the next Node/Channel record waits for the previous handler's terminal. Execution-exclusion decision sites for Node/Channel go 3→1 (one FIFO head take).
- Keep: the owner queue, host permits, ready membership, relocation seal; Spot/Actor gates and their relocation-visible prefix stay as they are (design line 338). Keep the existing coroutine invoker; coroutines stay; do not replace any coroutine with a callback or a hand-written state machine. Do not change worker count, wake rules, dispatcher objects beyond what the claim removal requires, coroutine frames or W5 (later steps).
- Reuse binding features; do not reinvent what the binding provides (design §3).
- Lessons from failed attempts (design §6b): cdrain (independent take + multi-worker drain) cost send −11.4% and Spot p99 501→7,586 µs; csendb doubled same-rate CPU. Do not use more workers or more drains as the saving; the saving must come from deleting the claim/batch/settlement objects. Before you write code, write in the report (section 0) how this change differs from cdrain in wake count, lock count and per-message allocations, with file:line. If you cannot show a difference, stop and report instead of implementing.

## Tests (each must fail on main or with a negative control, then pass with the change)
- Count test per Node message: prefix claim allocation 0, settlement RMW 0, queue commit 1 and take 1. Negative controls: re-adding one claim allocation, one settlement RMW, one extra take must each be caught.
- Concurrency: on one Node, handler A blocks synchronously while B proceeds on another worker; FIFO take order; last-take vs enqueue race; shutdown while records are queued and while a handler runs; reply after shutdown. Spot/Actor gate negative controls stay and still fail when the gate is removed.
- Full C++ Framework unit and contract suite (non-install entries) with pass/fail counts and logs. The receive/route/mailbox/Spot tests 20 times in a loop under `stress-ng --cpu 16 --cpu-load 80` via /root/jobs/fx/cx/measure-shared.sh; report any failure with its log.

## Measurement (PR fails on regression)
Do not run full throughput tables; the supervisor measures cells. After tests pass:
- Same-rate target user+kernel CPU per message, send-saturation 1024, base (this branch's parent 26d5c811c7) vs change, 3 runs each, with /root/jobs/fx/cx/v21/s033/csrc/scripts/measure.py (copy into p2/scripts, adjust paths). Compare with the design estimate 0.8–1.4 µs/message.
- Regression checks the PM set as PR failure conditions: Spot request p99 (1024, base vs change, 3 runs, median) and send-saturation 1024 throughput (3 runs, median). Any worse median beyond the run-to-run spread you also report is a failure; say so plainly.

## Rules
- Do NOT build, test or take any lock until the file /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (a Core release gate is measuring). Until then, read and write code only.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` called bare (no outer flock), linux-ninja-release preset, no -g; keep p2/ plus build dirs under 25 GB (`du -sh` after each build). Compile all affected targets with -k 0 before any test. Tests through /root/jobs/fx/cx/measure-shared.sh. Measurements inside `flock -o /root/jobs/fx/cx/measure.lock`, MemAvailable >= 12 GiB, source CPUs 0-7, target 8-15. SIGTERM/SIGINT only; no background jobs with &.
- Keep every raw output under p2/raw/; never delete them.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only the worktree /root/worktree/zlink-v21-s033/p2 (branch perf/1639-cpp-p2-queue-head at 26d5c811c7), /root/jobs/fx/cx/v21/s033/cpath/, /root/jobs/fx/cx/v21/s033/p1/report.md, /root/jobs/fx/cx/v21/s033/cdrain/ and /root/jobs/fx/cx/v21/s033/csrc/scripts/measure.py. Write only in /root/jobs/fx/cx/v21/s033/p2/ and the worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- Run `bash scripts/format/format.sh --check cpp` in the worktree before finishing and fix what it reports.
- 리팩토링 점검: list the claim, batch, settlement, dispatcher and wrapper sites on the Node/Channel receive path with file:line and whether this change removed them; before/after counts of execution-exclusion decision sites (3→1) and per-message allocations.
- Time box about 8 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p2/report.md (Korean, plain sentences)
(0) difference from cdrain; (1) what changed with file:line; (2) decision-owner table and before/after counts; (3) tests with negative controls and logs; (4) same-rate target CPU base vs change; (5) Spot p99 and send throughput base vs change; (6) 리팩토링 점검. Save the diff against 26d5c811c7 as p2/change.diff.
