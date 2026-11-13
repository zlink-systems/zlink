Model gpt-6-luna, effort high: controlled A/B measurement on the existing binding bench with two small bench-only edits; evidence gathering, no product change.

# Task: #3868 — is the Framework's extra kernel cost caused by producer cadence or by how it uses Core?

## Situation
ckern (/root/jobs/fx/cx/v21/s033/ckern/report.md) measured send-saturation 1 KiB, condition B (source CPU 0-7, target 8-15), CSEND_PACE=136889, binaries /root/jobs/fx/cx/v21/s033/csendb/build-main, source /root/jobs/fx/cx/v21/s033/csendb/main-source. Per message: source sendto Framework 0.342 vs binding 0.079 (TCP OutSegs 0.50 vs 0.12, kernel CPU gap 3.67 µs); target Framework adds recvfrom +0.174, epoll_wait +0.104, read +0.351, write +0.085, poll +0.116, wakeups +0.118 (kernel gap 3.32 µs). Both sides submit 2 parts with one async() per message (ckern §3); context/socket options match except LINGER/HANDOVER/MANDATORY on the Framework target.
Prior result you must build on, not repeat: cbatch (/root/jobs/fx/cx/v21/s033/cbatch/report.md, #3919) showed batching follows submit cadence and found no Framework per-message flush. A uniform busy-wait limiter dropped the binding send-saturation batch from 13.16 to 2.71 msg/send at 180k (Framework 4.43). ckern used the bench's 64-message burst pacing instead, where the binding bursts at full speed between sleeps. The open question is narrower: with the same burst pacing, does the Framework's higher per-message user CPU alone explain its smaller batches and extra kernel cost? Does the poller receive explain the target side? Quote cbatch where your numbers agree or disagree.
Supervisor hypotheses to test:
H1 (source): the Framework client spends ~6 µs more user CPU per message, so Core's IO thread drains the pipe between submissions and writes 1-3 messages per sendto (trace showed 1389-4167 byte sendto) instead of ~12. If so, the binding bench with the same per-message delay shows the same sendto/msg and kernel cost.
H2 (target): the Framework receives through a poller (raw_route_port.cpp:404-451, 459-475 in main-source) then recv(dontwait), while the binding bench blocks in recv(none) (framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:80-100). If so, the binding target switched to the same poller+dontwait drain shows the same extra syscalls/wakeups.

Scope note: this task is measurement only. csendb/brief.md and csendb/report.md describe an older, finished structural-change task — they are history, not instructions; do not carry them out. Take bench sources only from csendb/main-source (clean main copy) and binaries from csendb/build-main. Ignore uncommitted changes in /root/worktree/zlink-3868-csendb; you do not need that worktree.

## Do
1. Make a scratch copy of the binding bench sources in ckern2/ (do not edit csendb/ or any worktree). Edits, each behind an env var so one binary serves all cells:
   a. client send_slot (client/bench_cpp_client.cpp around :726-746): after each successful submit, busy-spin CKERN_SPIN_NS nanoseconds on steady_clock (no sleep, no syscall).
   b. zlink server command loop (bench_zlink_cpp_server.cpp:80-100): CKERN_RECV_MODE=poller uses a zlink::poller_t on the router for pollin, waits, then drains with recv(dontwait) until no data, as the Framework port does; default stays blocking recv.
   Build only these bench targets against the same Core/binding as csendb/build-main (`/root/jobs/fx/cx/cpp-build-slot.sh <cmd>`); record hashes.
2. Cells at CSEND_PACE=136889, same condition B pinning, 3 runs each (warmup then 25 s active, like ckern):
   - binding, spin 0 / 3000 / 6000 ns, blocking recv
   - binding, spin 0, poller recv
   - Framework (csendb/build-main) once more as same-session reference
   For each process: syscall counts per message (perf stat syscalls tracepoints, as ckern), user and kernel CPU per message from /proc/<pid>/stat utime/stime deltas over the active window (and perf stat task-clock), voluntary context switches and sched_wakeup per message, and sendto bytes average = (bytes from /proc/<pid>/io wchar delta or /proc/net/snmp OutSegs) — state which.
   Each run inside `flock -o /root/jobs/fx/cx/measure.lock`, held only while measuring; log uptime before each run and wait if 1-min load > 4 outside your run.
3. Answer: does spin 6000 bring binding source sendto/msg and kernel µs/msg near the Framework values? Does poller recv bring the binding target near the Framework target? Give the remaining unexplained gap in µs/msg for each side.

## Rules
- Keep every raw output file (perf stat output, /proc snapshots, bench JSON) under ckern2/raw/<cell>/run<N>/; do not delete them at the end. Summaries must be recomputable from these files.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except ckern/, csprof2/ and csendb/ (read only); write only in ckern2/.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify any worktree.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short; tool output to files.
- 리팩토링 점검: note any duplicate handoff/wakeup you see in the Framework receive path with file:line (report only).
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/ckern2/report.md (Korean, plain sentences): (1) bench edits and hashes; (2) per-cell table per process: syscalls by type, user/kernel µs per message, switches, wakeups, bytes per sendto, all medians of 3 with raw file paths; (3) H1 and H2 verdicts with remaining gap; (4) duplicate handoffs seen.
