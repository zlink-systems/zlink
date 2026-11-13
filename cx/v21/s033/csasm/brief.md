Model gpt-6-luna, effort high: the change already exists in a reviewed diff; this task extracts one part of it, keeps tests, and measures. Files and rule are pinned.

# Task: C++ send — keep only the source single-assembly part of the csendb diff and measure it (#3868)

## Situation
Job csendb (/root/jobs/fx/cx/v21/s033/csendb/report.md and change.diff, based on main a0056577c4) combined two changes:
(A) target record-independent execution: service_mailbox.cpp/.hpp, offload_executor.cpp/.hpp, mesh_node_host_service.cpp, application_record.hpp and related tests (independent_node_records, mailbox_owner_dispatch, mailbox_fifo_cost, claim_dispatch_cost ...). The PM and supervisor decided to DROP (A): it doubled target CPU without throughput gain.
(B) source single assembly: the remote Node send writes the final service payload once (header native buffer then copy removed; buffer allocations 2 -> 1). Files around channel_runtime.cpp:1441,1485, envelope_codec.cpp:357,452,528,1316, service_wire_codec.cpp:3224, client_call_codec, serializer.hpp, channel.hpp, mesh_node_runtime.cpp:4252,4342, public_host_runtime, app.cpp and their tests (envelope_allocations, send_allocations, request_wire_identity, route_payload_ownership ...). See report section 2 and the 중복 작업 점검 table.
Worktree: /root/worktree/zlink-3868-srcasm, branch perf/3868-cpp-send-single-assembly, at latest main f784975639 (main moved since a0056577c4).

## Do
1. Port only (B) onto the worktree. Decide per hunk whether it belongs to (A) or (B); list each file with the decision. Hunks of mesh_node_runtime.cpp that only serve (A) stay out. No hunk of service_mailbox / offload_executor / mesh_node_host_service unless it is needed by (B) — say why if so.
2. Build and run the full C++ unit/contract CTest (all targets compile, -k 0 style). Builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare. No long -j1 full builds.
3. Measure main (f784975639) vs (B) in the same session under condition B (source CPUs 0-7, target 8-15), reusing csendb/scripts (measure.py, collect.py, check_results.py) and csendb/bench.diff (pins and pacing, applied only to a scratch copy or reverted after): send-saturation 1 KiB and 4 KiB, request-backpressure 1 KiB, request-serial 1 KiB, 5 runs each, alternating order, binding in the same session. Plus same-rate total CPU (source+target, user+kernel) for send-saturation 1 KiB at the csendb common rate, 3 runs each. Measurements only inside `flock -o /root/jobs/fx/cx/measure.lock <cmd>`, held only while measuring. Another job (csprof) also measures; take the lock normally and wait.
4. Report F÷B (Framework throughput ÷ binding throughput, higher is better), Framework µs/message, budget = binding µs/message ÷ 0.6, and over-budget µs/message per cell, main vs (B).
5. Decision-site count for (B) before/after (allocation sites 2 -> 1 expected), and git diff --check, `scripts/format/format.sh --check cpp` if it exists.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except csendb/; write only in csasm/ and this worktree.
- No git operations except git diff and git status (do not commit); no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- MemAvailable >= 12 GiB before each build step. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- 리팩토링 점검: list duplicate conversions/copies on the send path you see with file:line and whether (B) removed them.
- Time box about 5 hours.

## Output: /root/jobs/fx/cx/v21/s033/csasm/report.md (Korean, plain sentences): (1) per-file (A)/(B) decisions; (2) tests with log paths; (3) measurement table main vs (B) and same-rate CPU; (4) decision sites; (5) duplicate work. Save the full diff (including new files) as csasm/change.diff.
