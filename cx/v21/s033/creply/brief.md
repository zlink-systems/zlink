# Task: C++ Framework reply submission — does the C++ reply path send replies in more, smaller TCP packets than the binding, as Java did? (#3867, measurement-only)

## Situation
Java finding (/root/jobs/fx/cx/v21/s033/jsend/report.md — read fully, reuse its runner/tools under jsend/runner and jsend/tools): at the same rate, the Framework reply left in 1.8x more TCP data packets per request than the binding (0.544 vs 0.300), and kernel cost per packet was the same, so packet count explained ~93% of the extra send cost. Cause: replies were serialised through a Framework port state lane, so they reached the socket spread out. Submitting directly through the binding reply (concurrent submit on one handle is allowed by bindings/doc/spec/README.ko.md "Multipart 제출") gave +27.7% throughput on that copy and +9% on main.
C++: framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:501 submits the reply inside a socket lifetime mutex. C++ request-backpressure 1KiB is F÷B 0.207 (Framework ~19.4 µs/request caller-bound; binding 249k req/s). Earlier C++ studies: cprof, cfloor, cstage reports in /root/jobs/fx/cx/v21/s033.
Worktree: /root/worktree/zlink-creply = main d961e51e23 + CPU-pin patch.

## Do
1. Fix the C++ reply path with file:line (target handler → reply build → raw_route_port reply submit → binding → Core I/O thread) and which thread runs each step; say what the mutex at raw_route_port.cpp:501 protects, and whether the binding/Core already owns that decision (close gate, same-handle concurrency).
2. At the same rate (pace both to a rate both reach, e.g. 50k req/s), request-backpressure 1KiB, Framework vs binding: reply data packets per request, average segment size, ACKs per request, target reply send kernel cycles/request and per packet, sendto calls and bytes per call, wakeups per request on the target I/O thread, mutex contention (wait time per request on that mutex).
3. If the packet pattern matches Java's (more, smaller reply packets) or the mutex shows contention: prototype submitting the reply directly through the binding without the Framework mutex (keep close safety via the binding/Core close gate; say exactly how), and measure unpaced RUNS=5 request-backpressure 1KiB and 4KiB and request-serial 1KiB with the binding in the same session, plus the packets/request at the same rate. If it does not match, say so and stop there.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree (measurement copy, never committed).
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (no outer flock). Other builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; wait if held — other measurements are queued), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work (same check/encode/copy/handoff twice) you see, with file:line, and whether your prototype removed it; no new queue, lock, cache, flag or state in any prototype.
- Report "over budget by X µs" per message, never as a percentage of the target. Do not suggest lowering the 0.6 target.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/creply/report.md (Korean, plain sentences): (1) path and mutex ownership; (2) same-rate packet/CPU table; (3) prototype and numbers or the reason to stop. Save any prototype as creply/prototype.diff.
