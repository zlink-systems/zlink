# Task: C++ vs Java vs .NET vs Node Framework — same cells, same session, per-message segment numbers side by side, against the per-cell reference language (measurement-only)

## Situation
The target stays Framework÷binding ≥ 0.6. This is a diagnostic: by absolute numbers the C++ Framework structure looks wrong, and per-cell gaps between languages look odd. Per cell, the language with the best F÷B (11-01 table) is the reference structure: send-saturation = .NET (0.78/0.83), request-backpressure = Node (0.61/0.70), request-serial = Java (0.62/0.63). Two C++ breakdowns are running in parallel and you must not touch their worktrees: /root/jobs/fx/cx/v21/s033/csend (C++ send-saturation 1KiB, #3868) and /root/jobs/fx/cx/v21/s033/creply (C++ request-backpressure reply path, #3867). Your job adds the Java, .NET and Node Framework numbers for the same cells, in the same session as a C++ Framework run, so they can be put side by side.
Last numbers (different sessions): send-saturation 1KiB Framework C++ 181,862 msg/s; request-backpressure 1KiB Framework C++ ~51.5k, Java ~62.5–68k, .NET ~36.5k req/s.
Reusable method and tools: Java /root/jobs/fx/cx/v21/s033/jio, jenv, jsend (runner/, tools/: schedstat per thread, perf, BPF, JFR splits); .NET /root/jobs/fx/cx/v21/s033/dseg4, dhop, dcount, dprof, nser3; C++ cprof, cfloor.
Worktree: /root/worktree/zlink-xlang = main d961e51e23 + CPU-pin patch.

## Do
0. A previous run of this job was stopped after building; reuse its outputs in this directory (bin/, build-*.log) when they are still valid for this worktree, and build only what is missing (Node).
1. Build C++, Java, .NET and Node benches in this worktree (C++ via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare; Java/.NET via `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash <lang>/build.sh` from framework/bench/grpc). C++ package via scripts/local-package/build-wsl.sh cpp if the bench needs it (through cpp-build-slot.sh).
2. In ONE session under `flock -o /root/jobs/fx/cx/measure.lock` (hold it across the whole session; condition B source CPUs 0-7, target 8-15), cells send-saturation 1KiB, request-backpressure 1KiB and request-serial 1KiB, for each of C++/Java/.NET/Node Framework and its binding: unpaced RUNS=3 median and range (msg/s or req/s, µs/msg).
3. Same session, paced to one common rate every implementation reaches (per cell), per message: total CPU across both processes (user+kernel), CPU of each thread role (caller/submit, Framework dispatch/mesh, handler, Core I/O send, Core I/O receive, completion), thread handoffs per message, allocations per message (bytes and count: JFR allocation/TLAB or GC counters for Java, GC.GetAllocatedBytes / EventPipe for .NET, malloc counting via uprobe or LD_PRELOAD counter for C++, heap/allocation sampling or --trace-gc counters for Node — state the method), payload-size copies per message (count body passes in the code path, file:line), TCP data packets per message.
4. One table per cell: rows = the measures above, columns = C++ F, Java F, .NET F, Node F, then the four bindings. Mark the reference language column. Then for every other language: µs/msg gap of its Framework layer (F µs/msg − own B µs/msg) vs the reference language's Framework layer, and which rows carry it; list the steps the reference does not have, largest first.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify bindings/ or core/. Instrumentation only in bench files or overlays; no product change.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- Report gaps as µs per message; never suggest lowering targets. Node bench may need its own build script under framework/bench/grpc/node.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/xlang/report.md (Korean, plain sentences): (1) unpaced table; (2) per-cell side-by-side tables; (3) per-language Framework-layer gap vs the reference and the rows/steps carrying it; (4) method notes.
