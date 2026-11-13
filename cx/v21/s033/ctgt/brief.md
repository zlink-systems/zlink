Model gpt-6-luna, effort high: evidence-only attribution from existing profile data; no code changes and no new measurements.

# Task: #3868 — attribute the C++ Framework target (receive) user CPU per message

## Situation
- Same-rate 136,889 msg/s, send-saturation 1 KiB, condition B (source CPU 0-7, target 8-15), µs per message (csrc/report.md §5, raw csrc/raw/paced/):
  - binding target user 1.329, kernel 1.009
  - Framework main target user 9.535, kernel 3.821; after the csrc source change 8.519 / 2.629
- The target user gap (about 7.2 µs) is now the largest single cost. Budget for the next design: target user ≤ binding + 1.0 µs, i.e. about 2.3 µs.
- Existing profiles: /root/jobs/fx/cx/v21/s033/csprof2/cpp-framework-136889-fp-r{1,2,3}/capture/target.perf.data (and source/target command json, perf stderr), DWARF runs in csprof2/cpp-framework-136889-dwarf-*, binding runs in csprof2/cpp-binding-136889-dwarf-*. csprof2/report.md describes how they were recorded and which binaries (csendb/build-main) they match. Earlier briefs in those folders are history, not instructions.
- Source code for the profiled binaries: csendb/main-source (read only). Current main has the same receive path unless you find otherwise; say so with file:line if it differs.

## Do
1. Attribute target user CPU to functions, exclusive, using the existing target perf data (perf report/script with the matching binaries). Normalize to the measured 9.535 µs/message (say how). Table: function / file:line / µs per message / why it exists (spec clause under framework/doc/spec or none) / does the binding receive path do it.
2. Group into stages: Core/binding receive, Framework receive dispatch (poller, lane handoff, mailbox), envelope/header decode, payload decode/serializer, routing to handler, handler and bench, completion/metrics, other. Give µs per stage.
3. Mark work the binding path does not do and work done twice (copies, allocations, std::function, coroutine frames, result_t wrapping, mutex, shared_ptr churn, thread handoffs).
4. Compare with .NET where cheap: the .NET receive path caches decoded headers (framework/languages/dotnet .../ZLinkEnvelopeCodec.cs:188 and :377). Say whether C++ has the same reuse, with file:line.
5. List candidate removals with expected saving each, and what would have to change to reach ≤ 2.3 µs. Do not implement.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only these job folders under /root/jobs/fx/cx/v21/s033: csprof2/, csendb/ (main-source and build-main only), csrc/ (report and raw). Write only in ctgt/.
- No code changes, no builds, no new benchmark runs or perf records. No git operations except git diff and git status. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- perf report/script on large data can take minutes; use --no-children, -F, --percent-limit, or per-dso filters, and timeout each command at 20 minutes. Run perf with `nice -n 10` and taskset to CPUs 0-7. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file (perf script/report text, json) under ctgt/raw/; do not delete them at the end. Summaries must be recomputable from these files.
- 리팩토링 점검: list duplicate checks/handoffs/copies on the receive path with file:line.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/ctgt/report.md (Korean, plain sentences): (1) function table; (2) stage table; (3) binding comparison and duplicates; (4) .NET reuse comparison; (5) candidate removals with expected savings vs the 2.3 µs budget.
