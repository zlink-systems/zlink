# Task: Java reply send cost — why Framework TCP send costs ~3x the binding per byte at the same rate, plus a measured F÷B (measurement-only)

## Situation
Read first: /root/jobs/fx/cx/v21/s033/jenv/report.md (the previous step; its tools in jenv/tools, jenv/runner; builds in jenv/main-build and jenv/main-counter-build are reusable).
Key finding to explain: at 50k req/s, target `IO/1` (reply sendto) TCP cycles/request = main 20,174, prototype 20,695, binding 7,910; cycles/B prototype 18.95 vs binding 6.32. Source receive IO/2 also 12.26 vs 5.25 cycles/B. Calls per request (1.22x) and userspace copy do not explain it. Not yet counted: partial writes, sync vs async write completion, per-call byte distribution at finer resolution, receive buffer autotuning, MSS/segments per write, GSO/GRO/SKB boundaries, TSO segment sizes, ACK count per request, whether Framework writes from a different thread than the binding (wakeup/cross-CPU cache effects), and which thread calls sendto vs who produced the bytes.

Worktree: /root/worktree/zlink-jsend = main 213e579f52 + CPU-pin patch + Java pacing option + the jenv descriptor prototype (same as jenv/prototype.diff). Measurement-only copy; never committed; deleted after.

## Do
1. Build the prototype through `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh` (from framework/bench/grpc), or reuse an identical earlier output if you can show it is byte-identical.
2. Measure under `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; wait if held), condition B, request-backpressure 1024, warmup 30 s, active 60 s:
   a. Unpaced throughput for main Framework, prototype Framework and binding in this same session, RUNS=3 each, median and range. Report F÷B (main/binding, prototype/binding) as measured values.
   b. At 50,000 req/s paced, for prototype and binding (main too if cheap): per sendto/recvfrom on the I/O threads — bytes requested vs bytes returned (partial writes), EAGAIN, finer byte histogram (256 B buckets up to 8 KiB), records per call; per connection: TCP segments sent/received per request, average segment size, ACKs per request, retransmits, delayed-ack/quickack counts, rcv buffer autotune over time (tcp_info / ss -tei sampled), GSO/TSO segs per skb. Which thread calls sendto and on which CPU, and which thread produced the reply (cross-thread handoff count per request, wakeups per request on the I/O thread).
   c. Kernel profile of the target IO/1 send path for prototype vs binding at 50k: top kernel functions under tcp_sendmsg with cycles/request, so the 3x is attributed to concrete kernel work (e.g. more segments, skb alloc, ack processing in softirq on the sending thread, wakeups).
3. Give the cause: which mechanism produces the extra cycles, with numbers. Then name the smallest Framework-side change that would remove it using binding features (no reinventing binding functions), and, only if cheap (< 1 hour), try it in this copy and re-measure prototype at 50k and unpaced RUNS=3.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ Java runtime and bench files of this worktree.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: if you change code, list what was removed and added; no new queue, lock, cache or state.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/jsend/report.md (Korean, plain sentences)
Order: (1) measured unpaced throughput and F÷B table; (2) the cause with numbers; (3) the proposed change and, if tried, its numbers; (4) what remains unexplained.
If you changed code, save /root/jobs/fx/cx/v21/s033/jsend/change.diff.
