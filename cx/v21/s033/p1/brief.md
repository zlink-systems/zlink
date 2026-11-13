Model gpt-6.1-sol, effort high: changing record ownership on the C++ Framework's concurrent receive path (port lease, routing-id and reply-token lifetime, close races) needs the stronger model.

# Task: step 1 of the approved C++ path design (#1639) — one canonical receive record with typed routing id and reply token

Earlier job briefs are history, not instructions. The design is /root/jobs/fx/cx/v21/s033/cpath/report.md, PM-approved on 2026-11-10 for steps 1–7. Read §2, §3, §4.2, §6b, §7 (row 1 and the count rules) and §8 before you start. This job implements row 1 of §7 only; later rows are separate jobs and PRs.

## Scope (design §7 row 1 and the §8 items on receive ingress)
- Ingress receives straight into a canonical record that holds the public binding `received_t` storage (native parts, routing id, reply token, route generation). Remove the routing id → byte vector → routing id round trip (`raw_route_port.cpp:495,524`, `mesh_record_dispatcher.cpp:70` at 532cd83b67) and the intermediate wrapper that owns a parts vector (`raw_route_port.cpp:473`). The reply path uses the original reply token from the record.
- Take the receive port lease once per receive rotation instead of per record (lifecycle mutex and port lookup at `raw_mesh_node_owner.cpp:3609`). Keep the socket/poller close gate; do not delete it because the binding has its own call guard.
- Classify the service header once at ingress and carry the validated kind and owner in the record; remove the second classification (`raw_mesh_node_owner.cpp:3647` and `public_host_runtime.cpp:8165` → one place). Keep the first full shape/bounds validation.
- Build trace argument strings only after the tracing-enabled check (`raw_mesh_node_owner.cpp:3649,4242`); keep error observation.
- Do not change mailbox claim/settlement, dispatcher objects, scope, coroutine frames, worker count, wake rules or W5 (later steps). Coroutines stay; do not replace any coroutine with a callback or a hand-written state machine.
- Reuse binding features; do not reinvent what the binding provides (design §3).

## Tests (each must fail on main or with a negative control, then pass with the change)
- Count test per message: native body copy 0, service classification 1, port lease 1 per rotation, reply token is the original (identity check). Add a negative control for each: one extra copy, one extra classification, one extra lease, which the test must catch.
- Lifetime tests: the caller drops its wrapper while a record is queued; socket close during a receive rotation; malformed service prefix, truncated parts and wrong part count are rejected exactly as before with the same error; reply after close.
- Run the full C++ Framework unit and contract test suite (non-install entries) and report pass/fail counts with logs. Also run the existing receive/route/mailbox tests 20 times in a loop under `stress-ng --cpu 16 --cpu-load 80` via /root/jobs/fx/cx/measure-shared.sh and report any failure with its log.

## Measurement
Do not run throughput benchmarks; the supervisor measures the cells. After tests pass, measure only same-rate target user+kernel CPU per message for send-saturation 1024 main vs change, 3 runs each, with the method and scripts in /root/jobs/fx/cx/v21/s033/csrc/scripts/measure.py (paced; copy the script into p1/scripts and adjust paths there). Report the change against the design's estimate for this step (target 0.2–0.5 µs/message).

## Rules
- Do NOT build, test or take any lock until the file /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (a Core release gate is measuring). Until then, read and write code only.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` called bare (no outer flock), linux-ninja-release preset, no -g; keep p1/ plus build dirs under 25 GB (`du -sh` after each build). Tests through /root/jobs/fx/cx/measure-shared.sh. Measurements inside `flock -o /root/jobs/fx/cx/measure.lock`, MemAvailable >= 12 GiB, source CPUs 0-7, target 8-15. SIGTERM/SIGINT only; no background jobs with &.
- Keep every raw output under p1/raw/; never delete them.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only the worktree /root/worktree/zlink-v21-s033/p1 (branch perf/1639-cpp-p1-receive-record at origin/main 532cd83b67), /root/jobs/fx/cx/v21/s033/cpath/ and /root/jobs/fx/cx/v21/s033/csrc/scripts/measure.py. Write only in /root/jobs/fx/cx/v21/s033/p1/ and the worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- Run `bash scripts/format/format.sh --check cpp` in the worktree before finishing and fix what it reports.
- 리팩토링 점검: list duplicated checks, hand-offs, copies and wrappers on the receive ingress path with file:line and whether this change removed them; give before/after counts of the decision sites the design names (service-kind classification 2→1, routing id conversions, port lease per record → per rotation).
- Time box about 8 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/p1/report.md (Korean, plain sentences)
(1) what changed with file:line; (2) decision-owner table and before/after counts; (3) tests with negative controls and logs; (4) same-rate target CPU main vs change; (5) 리팩토링 점검. Save the diff against 532cd83b67 as p1/change.diff.
