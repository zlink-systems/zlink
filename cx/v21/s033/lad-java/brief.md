Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 ladder L0 + cross test — Java (Kotlin not needed for L0). EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java (experiment branch; commit locally on it, never push, never open a PR, never merged).
Read first, verbatim decisions:
- /root/jobs/fx/cx/v21/s033/issue-4350.md (this step, the common method, the budget and the measurement conditions)
- /root/jobs/fx/cx/v21/s033/issues-4351-4357.md (the later steps, so L0 is built so that L1..L7 can be restored one at a time)
- baseline table: /root/jobs/fx/cx/v21/s033/r4190w/report.md (r4190w, 30 s warmup)
- /root/jobs/fx/cx/v21/s033/d3868/report.md (C++ costs; for orientation only)
- Java Framework: framework/languages/java; bench framework/bench/grpc/java. The 2026-09 #56 Java ladder reported: permit 0%, envelope -6.5%, service wire -14%, queue/worker -10..-17%, activation -13%, send registry -74%; client side 37 pp vs server 19 pp. The code has changed since; re-measure, do not reuse those numbers.

## Do (L0 only in this job)
1. Strip the real Java (Kotlin not needed for L0) Framework route-mesh send and receive path to the binding perf shape (L0). The receiving owner blocks, drains until empty, and submits the received parts as-is in the reply; one fixed handler is called directly; the binding's own completion is used. Strip both the Framework bench server and the Framework bench client (framework/bench/grpc/<lang>). The existing bench and wire must keep working with raw/binding clients and servers. Use compile-time switches or conditional code ON THIS BRANCH ONLY. Record every turned-off item with file:line, grouped by the 7 ladder features (permit, envelope header, service wire framing, codec, owner queue·worker, activation·DI scope, operation registry·completion), so later steps can restore them one by one. Call the real Framework classes and mark anything imitated.
2. Measure L0 and current main (control) in the issue's conditions: kairos server, source CPU 0-7, target CPU 8-15, warmup 30 s, active 5 s, 3 runs, median, cells send-saturation, request-serial, request-backpressure × 1K and 4K, binding measured side by side. Reuse r4190w's runners and aggregator. Copy /root/jobs/fx/cx/v21/s033/r4190w/tools/run-group.sh into your job dir and change only the JOB/raw paths. Every run: errors 0 and submitted = completed = received; discard broken cells.
3. Cross test at L0 and at main (send-saturation and request-serial 1K): raw server + raw client, Framework server + raw client, Framework server + Framework client. This splits the loss between the source (client) side and the target (server) side.
4. Done criterion from the issue: L0 ≥ 0.95 × binding on every cell. If a cell is clearly lower, find the remaining cost location (file:line, with a profile or allocation/lock count) before reporting.

## DECISION UPDATE (PM 2026-11-12 08:30 UTC) — overrides the Do/Measure parts above
- Do NOT measure the main control in this L0 round. The control (L8) uses the 11-12 baseline at d3fd955131 (same conditions) as its reference value: /root/jobs/fx/cx/v21/s033/r4190w/report.md and raw. It is re-measured once, at the last ladder step.
- Measure only L0 and binding in the same session: 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), warmup 30 s, active 5 s, 3 runs.
- Cross test: at L0 only, send-saturation 1K only: raw server + raw client, Framework server + raw client, Framework server + Framework client. No cross test at main.
- Before EVERY measurement, write one line to progress.md: the built commit (`git rev-parse HEAD` plus a dirty flag), the worktree base (`git merge-base HEAD origin/main`), the binary paths and their sha256. Confirm that every binary was built from THIS worktree, never /root/zlink (an old checkout). Do not measure if they do not match.
- Keep all earlier raw files; mark earlier main/control files as not used.

## Rules
- Measurement only inside `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE, as run-group.sh does; build first, then chain build && verify binary && measure. Never measure a stale binary.
- C/C++ compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, -j3. Other languages build normally but outside the measure lock. Keep build output under the worktree; at most 25 GB; remove C++ build dirs at the end.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Raw outputs go in /root/jobs/fx/cx/v21/s033/lad-java/raw; never delete them.
- Write /root/jobs/fx/cx/v21/s033/lad-java/progress.md as soon as L0's first send-saturation 1K number exists, then continue.
- Time cap 6 h. Korean, short tables; every number cites its raw file.

## Output
/root/jobs/fx/cx/v21/s033/lad-java/report.md:
- L0 vs binding and main vs binding for all 6 cells (msg/s, ratio, 0.95 judgement)
- the cross test table
- the turned-off code locations per ladder feature
- the cost still left in L0 (if any)
- The Java L0 measurement 'l0-java' started by the previous attempt may still be running or finished (raw/bench/ and raw/l0-java-driver.log, lock holder cwd lad-java). Wait for it if it is still running (do not start a second one); use its results if its progress line can be reconstructed (commit + binary sha256 of this worktree's build) — otherwise re-measure.

## CORRECTION (kairos 2026-11-12 09:00 UTC) — L0 reply must do the binding server's reply work
- Java L0 run 1 showed request cells above binding (1.04-1.21) because the L0 server echoed the received parts back, while the binding server builds a new response-envelope Message, allocates a new body and re-encodes the BenchPayload with protobuf for every reply. That makes L0 do less work than binding, so the result is not a valid L0.
- The L0 server's reply path must do exactly what this language's binding (zlink-<lang> raw) server reply does: the same new envelope part, the same body allocation and protobuf encode, the same part count, the same submit call. Returning, forwarding or reusing the request message or its parts is forbidden. The receive side likewise must do the same decode work as binding.
- Before every measurement, put into progress.md the binding reply code (file:line excerpt) and the L0 reply code (file:line excerpt) side by side, with one line stating which allocations, copies and encodes each does per message. Measure only when they match.
- Request cells measured before this correction are invalid. Send-saturation cells are unaffected and stay valid.

## JAVA STATUS (kairos 2026-11-12 09:17 UTC)
- send-saturation L0 is done and valid: 3-run medians 1K L0 548,098.4 vs binding 554,016.4 (0.989), 4K L0 195,175.8 vs binding 199,386.2 (0.979), raw under raw/l0/bench/java/run1-3. Do not re-measure these. If the L0 send-saturation 1K cross test was not done yet, do it.
- Remaining: fix ZLinkFrameworkBenchServer.handleLadderL0 so the reply does the binding server's work (ZLinkRawBenchServer.java:115-123: Message.from(RawWire.RESPONSE_ENVELOPE) plus RawWire.encodeBenchPayloadMessage(body), two parts, closed after submit). Rebuild, record the parity lines and the commit/base/sha line, then measure only the 4 request cells (request-serial and request-backpressure at 1K and 4K) for L0 and binding in the same session, 3 runs, into raw/l0-reply/. Report medians and L0÷binding per cell.
