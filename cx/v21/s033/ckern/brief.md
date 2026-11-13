Model gpt-6-luna, effort high: read-only diagnosis with counters and a code comparison; files and binaries are pinned; no code change.

# Task: why the same Core send/receive costs ~3.3 µs/message more kernel CPU under the C++ Framework than under the binding pair (#3868)

## Situation
csprof2 (/root/jobs/fx/cx/v21/s033/csprof2/report.md §3-4, raw data in csprof2/) attributed CPU by call chain, send-saturation 1 KiB, condition B (source CPUs 0-7, target 8-15). At about the same rate (~159-175k msg/s):
- target Core/binding receive: Framework 6.902 µs/message (kernel 4.341) vs C++ binding pair 2.091 (kernel 1.020);
- source binding/Core send: Framework 7.282 (kernel 4.687) vs binding pair 1.979 (kernel 1.017).
Same libzlink, same machine. The Framework's own stages (decode, mailbox, dispatch) are close to .NET's; this kernel gap is the largest single item. One observed chain: reactive_socket_send_op_base::do_perform → __send → tcp_sendmsg → lock_sock_nested → spin lock slowpath (core/src/runtime/transports/tcp/tcp_transport.cpp:519, core/src/runtime/engine/asio/asio_engine.cpp:754).
Binaries: C++ Framework and binding bench in /root/jobs/fx/cx/v21/s033/csendb/build-main (main a0056577c4); source tree csendb/main-source. Reuse csprof2/scripts (or csendb/scripts) to run pairs with pacing and pins.

## Do
1. At one common paced rate (e.g. 136,889 msg/s, both pairs reach it) run Framework pair and binding pair, 3 runs each, inside `flock -o /root/jobs/fx/cx/measure.lock`. Per process and per thread, per delivered message: syscalls by type (`perf stat -e 'syscalls:sys_enter_*'` on the relevant ones: sendmsg/sendto/send, recvmsg/recvfrom/recv, epoll_wait, futex, eventfd/read/write, poll, clock_gettime) or `perf trace -s`; context switches (voluntary/involuntary); sched wakeups (`sched:sched_wakeup` count); TCP segments per message (from /proc/net/snmp OutSegs/InSegs deltas, or `nstat`), bytes per send syscall (sum of returned bytes / send calls if available via `perf trace`).
2. Compare Framework vs binding: which counts per message differ, and by how much. Translate the difference into kernel µs/message using the per-syscall cost you measure.
3. Code comparison (read only): how the Framework creates and configures its Core context and sockets vs the binding bench (framework/bench/grpc/cpp: the raw zlink server/client). List every context option (IO threads etc.) and socket option (HWM, linger, TCP options, batching/immediate-type flags, routing ids, socket types, number of sockets/connections between the two processes) with file:line on both sides, and how messages are submitted (per message flush or wakeup, DONT_WAIT, multipart parts count, extra command/control messages per data message). Use the binding spec where options are documented (bindings/doc/spec, core/doc/spec) — do not read core/include.
4. Conclude which difference(s) explain the extra syscalls/wakeups, with numbers, and name the Framework code that causes each one. If the counters do not explain the gap, say so and show what was ruled out.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except csprof2/, csprof/, csendb/; write only in ckern/.
- Do not modify any repository or binaries. No builds. No git operations except diff/status/log/show. No spec edits; no GitHub operations. Do not read core/include. Do not run scripts/dev/internal-docs.sh.
- Measurements only inside the exclusive lock, held only while measuring. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short; tool output to files.
- 리팩토링 점검: list duplicate work you see (extra wakeups, duplicate sends, extra control messages) with file:line.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/ckern/report.md (Korean, plain sentences): (1) method and rates; (2) per-message counter table Framework vs binding, per process/thread; (3) option and submit-pattern comparison with file:line; (4) explanation with numbers and owning Framework code; (5) duplicate work.
