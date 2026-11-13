Model gpt-6-luna, effort high: evidence-gathering profile with existing binaries and scripts; no design or code change.

# Task: where does C++ target CPU go per message in send-saturation 1 KiB (#3868)

## Situation
Job csendb (/root/jobs/fx/cx/v21/s033/csendb/report.md, read sections 4 and the paced CPU table) measured send-saturation 1 KiB under condition B. At the same rate (136,889 msg/s), total CPU per message: main source 8.875 + target 13.176 = 22.051 µs; binding source 2.595 + target 2.353 = 4.948 µs. Throughput budget at F÷B 0.6 is 2.652 µs/message wall; main is 5.606.
Main binaries already exist in /root/jobs/fx/cx/v21/s033/csendb/build-main (main a0056577c4, -O3 -DNDEBUG -g -fno-omit-frame-pointer). Source tree of that commit: /root/jobs/fx/cx/v21/s033/csendb/main-source (or the worktree's git objects). Measurement and perf scripts to reuse: csendb/scripts/measure.py, collect.py (it already drives perf), csendb/bench.diff (pacing and CPU pins).
Earlier related results (do not redo): cdrain2/report.md (gate-less multi-drain lost 12.5% on send), csend/report.md (budget analysis).

## Do
1. At the same paced rate (136,889 msg/s), record perf (frame pointers, both processes, all threads) for main Framework source+target and for the binding pair, 3 runs each, under exclusive measure.lock: `flock -o /root/jobs/fx/cx/measure.lock <cmd>`. Condition B: source CPUs 0-7, target CPUs 8-15; verify affinity.
2. For the Framework target, split CPU µs/message by thread (receive/IO, mailbox/drain, worker, other) and by stage: Core/binding receive, wire decode, envelope/route classification, mailbox enqueue/claim, dispatch/coroutine, handler, completion/terminal, allocation (malloc/free/new/delete), locks and futex/wakeups (kernel), other. Normalize sample shares to the unprofiled paced CPU median. Give the top 15 functions by self time with file:line in main-source.
3. Same split for the Framework source at a coarser level (submit, encode, binding/Core call, allocation, wakeups).
4. Compare each stage to the binding pair's cost for the same work; state for each stage the µs above binding.
5. .NET comparison (PM request: compare each stage with the best language's structure). .NET send-saturation 1 KiB is F÷B about 0.831. Profile the .NET Framework target (and source, coarse) at the same paced rate, 3 runs, with `DOTNET_PerfMapEnabled=1` so perf resolves JIT frames. Use the .NET bench in /root/worktree/zlink-ratio040/framework/bench/grpc/dotnet (commit 475305bebc; read run_local.sh and README.ko.md for how cells run). Build it only into an output folder under csprof/ (`/root/jobs/fx/cx/measure-shared.sh dotnet build -c Release -o /root/jobs/fx/cx/v21/s033/csprof/dotnet-bin ...`); do not leave build output inside ratio040. Measure the .NET binding pair too if its cost differs from the C++ binding pair. Put .NET stage costs in the same table next to C++ (same stage names), and mark each stage as: C++ only (no .NET counterpart), same stage but more expensive in C++ (by how much), or comparable. Give the .NET code location for each stage it has.
6. List which stages would have to be removed or shrunk to fit target+source into a total-CPU budget consistent with F÷B 0.6 (state your budget derivation), naming the code that does each stage. Do not propose a design beyond that list.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except csendb/, cdrain2/, csend/; write only in csprof/.
- Do not modify any repository (no tracked file edits in ratio040) or the binaries in csendb/. No git operations except git diff/status/log/show. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. No C++ builds; the only build allowed is the .NET bench into csprof/ as in step 5.
- Measurements only inside `flock -o /root/jobs/fx/cx/measure.lock`; hold it only while measuring, release between batches. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- 리팩토링 점검: list duplicate work you observe in the profile (e.g. the same conversion twice per message) with file:line.
- F÷B means Framework throughput ÷ binding throughput (higher is better).
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/csprof/report.md (Korean, plain sentences): (1) method and run list; (2) target stage table µs/message main vs binding; (3) source stage table; (4) top functions with file:line; (5) C++ vs .NET stage table with classification; (6) required removals list against the budget, saying for each whether it means removing the stage or replacing it with an existing binding feature; (7) duplicate work. Keep raw perf data in csprof/.
