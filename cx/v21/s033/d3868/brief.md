Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #3868 (+#3867) — C++ Framework send path diagnosis on current main. NO code changes.

Worktree /root/worktree/zlink-v21-s033/d3868 (detached at current origin/main c81c7b7314). Do not commit product changes.
Current gap (PM 30 s re-measurement): C++ send-saturation 1 KiB Framework÷binding 0.191; target ≥ 0.6. #3867 request/backpressure 0.165/0.253, serial 0.518/0.559.

## Read first (do not redo what they already established; say where main changed since)
/root/jobs/fx/cx/v21/s033/{csend,csendb,csasm,csprof2,ctgt,ctrecv,csrc,csrc2,cadvice,coreio,ckern2}/report.md. csendb tried Node-record independent execution + single assembly and got SLOWER (0.2839 → 0.2737, total CPU 22.05 → 45.12 µs/msg); cdrain2/csendb multi-drain failed. Main has since merged #4302/#4313 (receive roles at rotation boundaries, same-owner independent claim).

## Deliver (all on current main, C++ Framework send-saturation 1 KiB, plus the request path of #3867 where it shares code)
1. **Decision owner table**: for one sent message, every place that decides something (route/owner lookup, admission/budget, wakeup/notify, queue choice, completion, serialization) — file:line, what it decides, state it reads/writes, whether binding (bindings/cpp) already decides the same thing. Mark duplicates (Framework re-deciding what binding/Core already decides).
2. **Per-message allocations**: count heap allocations per message on source and target (e.g. a small LD_PRELOAD malloc/free counter or `valgrind --tool=dhat` on a short fixed-count run; Framework vs binding bench). Give count/message and the top call sites (file:line) with bytes.
3. **Locks per message**: mutex/futex/atomic-RMW sites on the path, file:line, acquisitions per message (perf `-e syscalls:sys_enter_futex` counts, uprobes on pthread_mutex_lock, or perf record call graphs), contended vs uncontended.
4. Which of 1–3 are shared with the #3867 request/reply submit path (perf bench request/backpressure, serial).
5. Per-message CPU budget table: budget from 0.6 × binding (total CPU/message, both processes, user+kernel, at equal rate) vs current, and which rows from 1–3 account for the excess. No design proposals beyond a one-paragraph list of candidate removals ranked by µs/msg; PM chooses the design.

## Rules
- Any timing or profiling run is a measurement: take /root/jobs/fx/cx/measure.lock EXCLUSIVELY (`flock /root/jobs/fx/cx/measure.lock <cmd>`) and pin source to cores 0-7, target 8-15 (condition B). Allocation counting under valgrind may run without the lock.
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release preset, ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON, Core prefix /root/.cache/zlink/core-release/1.25.0/linux-x64 (from bindings/cpp/VERSION), -j3, build dir /root/worktree/zlink-v21-s033/d3868/build-d3868, 25 GB cap; remove it at the end. Reuse csend measure/collect scripts where they fit.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Keep raw outputs in /root/jobs/fx/cx/v21/s033/d3868/raw; never delete them.
- Korean, short tables. Every number cites its raw file.

## Output
/root/jobs/fx/cx/v21/s033/d3868/report.md with sections 1–5 above, plus what main changed vs the earlier reports.
