Model gpt-6.1-sol, effort medium: second pass of a profiling task whose first pass (lighter model) was returned — stage attribution by call graph needs judgement about frame boundaries; no code change.

# Task: attribute every C++ and .NET target/source CPU sample to a Framework stage, send-saturation 1 KiB (#3868)

## Handoff from csprof (read /root/jobs/fx/cx/v21/s033/csprof/report.md first; its raw data is in csprof/)
Valid: method, binaries (csendb/build-main, main a0056577c4; .NET bench built into csprof/dotnet-bin from ratio040 475305bebc), pins, the paced total CPU baseline (main target 13.176, source 8.875; binding target 2.353, source 2.595 µs/message at 136,889 msg/s), thread split (target IO thread 4.41 µs, main thread 8.76 µs), the .NET perf-map method (`perf record -k 1`, DOTNET_PerfMapEnabled=1).
Returned by the supervisor, do not repeat:
1. Stages were assigned by the *self* symbol, so 7.43 of 13.18 µs on the target (and 5.56 of 8.88 on the source) landed in "other". libc, kernel, allocator, vdso and std:: samples must be charged to the Framework stage that called them.
2. Sampling was too coarse (-F 199, ~12 s): several rows rest on 15-22 samples, below the resolution of the 0.1-1 µs differences being compared.
3. .NET was unpaced while C++ was paced, so per-message CPU was not comparable (CPU per message is rate-dependent).
Also: std::sort in bench_stats_server.hpp:99 runs on stats polling, not per message; keep it as a bench line, not a Framework stage.

## Do
1. Profile at higher resolution: `perf record -e cpu-clock -F 4999 --call-graph fp` (frame pointers exist; use dwarf only where fp chains break, say where) for at least 20 s active per run, 3 runs each for C++ Framework, C++ binding, .NET Framework, .NET binding. Report sample counts per process and per stage.
2. Compare at equal rate. Use the unpaced natural rate for all four (C++ Framework ~178k, .NET Framework ~181k msg/s are close), and also keep the C++ paced 136,889 baseline. For each run record the actual delivered rate and process user+system CPU per message (both processes), so stage µs are normalized to measured CPU at that rate.
3. Attribution: walk each sample's call chain from the leaf upward and charge it to the first frame that matches a stage rule. Write the rule table yourself (stage -> function/file patterns, C++ and .NET), covering Core/binding receive, wire decode, route classification, mailbox enqueue/claim, dispatch/coroutine/pump, handler, completion/terminal, metrics/bench, send submit, encode, binding/Core send. Allocation, locks/futex and kernel are not stages: report them as a second dimension (inside which stage they happen). Unattributed samples must be under 10% of each process; list the top unattributed chains if not.
4. Table per process: stage µs/message for C++ Framework, .NET Framework, C++ binding, .NET binding, plus the alloc/lock/kernel split inside each stage. Mark each C++ stage: C++ only, same stage but costlier (by how much vs .NET), or comparable.
5. Give for each C++ stage over .NET the top 3 call chains (file:line in csendb/main-source) that carry the extra cost.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except csprof/, csendb/, cdrain2/; write only in csprof2/.
- Do not modify any repository or the binaries in csendb/ or csprof/dotnet-bin. No builds. No git operations except git diff/status/log/show. No spec edits; no GitHub operations. Do not read core/include. Do not run scripts/dev/internal-docs.sh.
- Measurements only inside `flock -o /root/jobs/fx/cx/measure.lock <cmd>`, held only while measuring. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short; perf report output goes to files.
- F÷B means Framework throughput ÷ binding throughput (higher is better).
- 리팩토링 점검: list duplicate work you see in the call chains (same conversion or copy twice per message) with file:line.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/csprof2/report.md (Korean, plain sentences): (1) method, runs, rates, sample counts; (2) stage rule table; (3) target and source stage tables with the alloc/lock/kernel split and unattributed share; (4) C++ vs .NET classification and top chains per costlier stage; (5) duplicate work.
