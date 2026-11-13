# Task: Java request-backpressure 1KiB — why the Framework makes Core I/O threads work 2x, and what the mesh thread duplicates (measurement-only)

## Situation
/root/jobs/fx/cx/v21/s033/jio/jsat-report.md (read first): at the same rate (57.5k req/s), per request, the Framework's
target `ZLINKbg/IO/1` costs 13.42 µs vs binding 7.67; source `ZLINKbg/IO/2` 13.02 vs 6.43; the source Framework mesh thread
(`zlink-jvm-raw-mesh-bench`) 11.92 µs with JFR top frames ZLinkServiceOperationRegistry.take (:516),
ZLinkJavaRawMeshNode.decodeRequestReply (:5293), ZLinkServiceM6AWireCodec.decodeMultipartView (:534). At the 0.6 rate (75.6k) these
threads reach ~1 core. jsat added a pacing option (`ZLINK_JAVA_BENCH_REQUEST_RATE`, accumulated target time, raw runRaw path) to the
Java bench in BenchDrivers.java / BenchOptions.java — that worktree is gone; re-add the same option here (bench files only).
Method to reuse for counters: /root/jobs/fx/cx/v21/s033/cbind/ and /root/jobs/fx/cx/v21/s033/coreio/ (report.md, tools/, runner/):
per-thread schedstat, BPF syscall counters per thread/FD (sendto/recvfrom calls, bytes, EAGAIN, epoll, eventfd reads/writes),
messages per sendto/recvfrom, wire decode of record sizes.

## Do
Worktree /root/worktree/zlink-jio (latest main with the CPU-pin patch; measurement-only; deleted after; never commit).
1. Re-add the pacing option. Build (`/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh` from framework/bench/grpc).
2. Under `flock -o /root/jobs/fx/cx/measure.lock`, condition B, request-backpressure 1024, RUNS=1 each: Framework unpaced, and the
   binding paced to the Framework's measured rate. For both: per I/O thread (source and target) CPU µs/request, sendto and recvfrom
   calls per message, messages per sendto/recvfrom, mean bytes per call, EAGAIN per message, epoll waits/returns per message,
   eventfd reads/writes per message by thread, context switches per message; CPU split of each I/O thread into TCP send, TCP receive,
   epoll, eventfd/wake, Core userspace (perf -g on those threads, as coreio).
   Answer (①): which of these explains the +5.75 µs (target IO/1) and +6.6 µs (source IO/2), in µs.
3. Wire bytes (also ①): decode one Framework request, reply and the binding request/reply; list the byte composition of the 175 B
   difference (service header, envelope/JSON header fields, metadata, wrapper lengths), with field names and sizes.
4. (②) Mesh thread: JFR (settings=profile) on the Framework source; split the 11.92 µs by frame (registry take, reply decode,
   multipart view decode, completion handoff, other) and for each say whether the binding already does that work for the same
   request (e.g. reply matching by correlation in the binding's pending map, multipart parsing the binding already did), with the
   binding API/file that does it. Facts and µs only, no design proposal beyond one line per item.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
  Do not modify bindings/ or core/. Only bench files may change, in this worktree only; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: not applicable (measurement-only, no product change).
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/jio/report.md (Korean, plain sentences; ① table first, then bytes, then ②)
