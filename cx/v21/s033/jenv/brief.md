# Task: Java prototype — Framework Node/Channel request on a native `[descriptor, payload]` wire, measured against main at the same rate (measurement-only)

## Situation
Read first:
- /root/jobs/fx/cx/v21/s033/jenv/design.md — the PM's design study. Section 3 row "(다-2)" is what you build; section 4 lists the TCP CPU hypotheses; section 5 states ownership (Core/binding: transport matching, reply token, completion, multipart; Framework: logical target, typed dispatch, end-to-end identity, flow, metadata, deadline).
- /root/jobs/fx/cx/v21/s033/jio/report.md — the last measurement (same rate ~51.9k req/s, request-backpressure 1KiB, condition B). Framework I/O threads: target `ZLINKbg/IO/1` 14.23 vs binding 7.19 µs/request, source `IO/2` 13.78 vs 6.06; mesh thread `zlink-jvm-raw-mesh-bench` 11.92 µs (registry take 1.19, reply header decode 1.09, multipart view decode 0.45, completion handoff 2.21, other 6.99). Framework request record 1,435 B = 13 B service header + 75 B multipart wrapper + 296 B JSON header + 1,027 B application part + 24 B ZMP framing; reply 1,443 B. Binding records 1,252 B = 201 B + 1,027 B + 24 B.
- Method and tools to reuse: /root/jobs/fx/cx/v21/s033/jio/ (tools/, runner/) and /root/jobs/fx/cx/v21/s033/cbind/.

Worktree: /root/worktree/zlink-jenv (main 213e579f52 + CPU-pin patch + the Java bench pacing option `ZLINK_JAVA_BENCH_REQUEST_RATE` already applied). Measurement-only copy; never committed; deleted after.

## Do
1. Binding comparison group: find what the binding bench's 201 B part is (bench source under framework/bench/grpc/java and the binding perf code it uses; one capture). State whether it is an application part the bench itself builds, with the file:line that builds it.
2. Prototype (Java Framework runtime only, Node/Channel request-reply path used by the request-backpressure bench):
   - Request record = native multipart `[descriptor, payload part]` sent through the binding's public request operation. The descriptor carries, once, what the service header and JSON header carry today for this path (operation kind, channel, message name, codec, 128-bit identity as 16 B, deadline if used); empty metadata and flow off may be encoded as absent. No 75 B wrapper, no packed length table, no JSON header.
   - Reply = native `[descriptor, payload part]` sent back on the original binding reply context; the requester gets the result from the binding's own request completion, so the per-hop service correlation echo and the registry lookup used only for hop matching are not needed on this path. Keep durable/end-to-end registry use where other paths need it.
   - Behaviour must be correct for this bench (all requests complete, no errors). Compatibility with other peers, other languages, Spot/Actor and ClientServer is out of scope; do not add a fallback that guesses between old and new formats per message.
   - Keep the change as small as the path allows; list every file changed with a one-line reason.
3. Build: from framework/bench/grpc run `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh` (and the Framework Java build it needs through the same wrapper).
4. Measure under `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; wait if held — other measurements are queued ahead), condition B, request-backpressure 1024, warmup 30 s, active 60 s, RUNS=1 each:
   a. main Framework: before changing any file, build the unmodified worktree once and copy its Java bench build output to /root/jobs/fx/cx/v21/s033/jenv/main-build/; run that copy as the "main" case,
   b. prototype Framework, unpaced and then paced to main's rate,
   c. binding paced to main's rate.
   For each: throughput, per I/O thread CPU µs/request with the jio split (TCP send, TCP receive, epoll, eventfd/wake, Core userspace), sendto/recvfrom calls and bytes per call, record sizes (decode one request and one reply), mesh thread CPU µs/request with the jio JFR split, and caller µs (jvm_thread_cores × 1e6 / throughput from results.json).
5. TCP CPU cause (design.md section 4, the three "높음" rows plus TCP_NODELAY): for main, prototype and binding at the same rate report bytes per write and per read distribution, native part count and sizes, the actual TCP_NODELAY/SO_SNDBUF/SO_RCVBUF on the connections, and kernel TCP cycles per byte. Say which row the numbers support and which they rule out.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
  Do not modify bindings/ or core/. Changes only in framework/ Java runtime and bench files of this worktree.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: in the report, list what the prototype removed (decode steps, wrapper, registry use) and what it added; no new queue, lock, cache or state.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/jenv/report.md (Korean, plain sentences)
Order: (1) one table main vs prototype vs binding at the same rate — record bytes, I/O thread CPU, mesh CPU, caller µs, throughput; (2) 201 B answer; (3) TCP cause rows; (4) files changed and what was removed/added.
Also save the prototype diff as /root/jobs/fx/cx/v21/s033/jenv/prototype.diff.
