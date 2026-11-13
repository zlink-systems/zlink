# Task: why does Core I/O cost 3–8× more per message under the Framework than under the raw binding? (issues #3867, #3868, #3870; part of #1639)

Evidence-only investigation across languages. Do not change tracked files.

## Observations (main ee11390257, condition B: source CPUs 0-7, target 8-15)
- C++ request-backpressure 1024 (/root/jobs/fx/cx/v21/s033/cprof/report.md): source Core IO thread 19.5 µs/message (Framework) vs 2.5 (binding); target Core IO 18.9 vs 3.3. Per message, source syscalls: sendto 0.88 vs 0.12, recvfrom 1.14 vs 0.31, read 1.42 vs 0.03, write 0.59 vs 0.02, epoll_wait 1.94 vs 0.35. Both benchmarks submit through Core `request(...).async()` (framework/bench/grpc/cpp/client/bench_cpp_client.cpp).
- Java send-saturation 1024 (/root/jobs/fx/cx/v21/s033/jprof/report.md): Framework source `ZLINKbg/IO/0` saturated at 10.1 µs/message (0.96 core) vs binding 3.46; target IO 8.3 vs 2.9.
- .NET request-backpressure 1024 (/root/jobs/fx/cx/v21/s033/dprof/report.md): Framework busiest threads are Core I/O (22–24 µs/message); syscalls 42.6/message source vs binding 28.3.
Hypotheses (unverified): the Framework sends more parts per message (service header + optional metadata + application envelope) than the bench's 2 parts; the Framework's submitter is slower so the IO thread finds one message per wake and never batches (the binding gets ~8 messages per sendto); the Framework adds per-message mailbox commands or poller re-arms (read/write 2.0/message looks like eventfd signalling); a separate completion connection for replies (RouteMesh uses a Completion connection, spec 08 §5) doubles the IO work; per-request completion entries in the binding's completion owner cause per-reply wakes.

## Goal
1. For C++ request-backpressure 1024 and send-saturation 1024 (and Java send-saturation 1024 for comparison), measure what the Core IO threads do per message under Framework vs binding: frames and parts per message on the wire (count sendmsg/sendto iovecs and bytes), messages per sendto/recvfrom (batching), eventfd/pipe reads and writes per message and who writes them (which thread, which Core/binding/Framework call), epoll_wait wakeups per message, number of sockets/connections in use and which carry data vs completion. Use perf trace/strace -f with -k stacks or bpftrace (uprobes on Core mailbox send/recv and zlink_msg_send) on the existing builds; attach only during the active window.
2. Attribute each extra cost to its cause with file:function on the Framework, binding or Core side.
3. Say which causes are Framework-side choices (fixable in Framework), which follow automatically from a slow submitter (batching), and which are Core/binding behaviour that the Framework triggers. For each Framework-side cause, propose the change and estimate µs/message saved on the Core IO thread.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Use the existing builds in /root/worktree/zlink-ratio040 (main + pinning) with SKIP_BUILD=1; do not rebuild there. Scripts and outputs under /root/jobs/fx/cx/v21/s033/coreio/ only.
- Every bench/trace run inside `flock -o /root/jobs/fx/cx/measure.lock ...` (other jobs measure too; wait). run_local.sh env: PATTERNS, PAYLOAD_SIZES=1024, IMPLEMENTATIONS, RUNS=1, WARMUP_SECONDS, DURATION_SECONDS, SKIP_BUILD=1, RUN_STAMP, OUTPUT_DIR under your job dir.
- Keep MemAvailable >= 12 GiB; delete raw traces over 1 GB after summarising.
- Time box about 2 hours; write the report even if items are missing ("not measured").

## Output: /root/jobs/fx/cx/v21/s033/coreio/report.md (exact path)
Per-message I/O table Framework vs binding, cause attribution with file:function, classification (Framework choice / batching consequence / Core-binding behaviour), proposed Framework changes with estimated savings.
