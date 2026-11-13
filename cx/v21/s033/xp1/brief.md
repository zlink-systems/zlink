Model gpt-6-luna, effort high: scripted perf/uprobe collection and small bench-only variants against a fixed plan with exact events, symbols and verdict table.

# Task: prepare (do not run) the Core I/O thread breakdown P1 and the bench-only bisection P2 (#1639)

Earlier job briefs are history, not instructions. The plan is /root/jobs/fx/cx/v21/s033/xp1/input/plan.md §6 P1 and P2 (read §0, §2 and §6 fully; the verdict table in P1 is what the scripts must produce numbers for). The previous measurement is /root/jobs/fx/cx/v21/s033/xio/report.md; reuse its pacing, capture and runner scripts in /root/jobs/fx/cx/v21/s033/xio/raw/ (run_local_capture.sh, runner_common.capture.sh, collect-*.py, analyze_*.py) and its bench pacing change in the worktree.

## This job prepares only. A Core release gate is measuring on this machine until /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists. Do NOT run any benchmark, perf, build, test or take any lock. Read and write files only. A second job will execute what you prepare.

## Prepare
1. P1 runner script /root/jobs/fx/cx/v21/s033/xp1/scripts/run-p1.sh that, per run and inside `flock -o /root/jobs/fx/cx/measure.lock`, with MemAvailable >= 12 GiB, source CPUs 0-7 and target 8-15:
   - runs these captures once each: request-backpressure 1024 at the xio equal-rate pacing (binding --target-rate 32000, Framework 48000, as xio equal22k), both at --target-rate 10000, and send-saturation 1024 at 10000 for both;
   - after warmup, finds the active Core I/O thread TIDs (ZLINKbg/IO/n with CPU, as xio did) in source and target and, per TID for the active window: `perf stat --per-thread -e` the syscalls and context-switches listed in plan §6 P1; `perf record -e cycles -F 4999 --tid=<tid>` (no -g); uprobe call counts on the eight symbols in plan §6 P1 in libzlink.so (use `perf probe -x <lib> --add` with the mangled or `nm -C` names; check each symbol exists with `nm -C <lib>` and list any that are missing or inlined instead of guessing);
   - writes per-capture raw files (perf stat output, perf.data plus a `perf report --stdio --sort symbol` text, uprobe counts, completed message counts) under xp1/raw/p1/<capture>/ and never deletes them;
   - removes the uprobes it added at the end, even on failure (trap).
2. Analyzer xp1/scripts/analyze-p1.py: per capture and per TID, counts per message for each event and uprobe; top-20 cycles self symbols; then a table that answers each row of the plan's P1 verdict table with the numbers that decide it (Framework/binding ratio of epoll_wait and on_read_complete per message, eventfd write per message on target and source, process_async_mailbox/process_commands/mailbox_t::send self time, set_local_receive_flow_state per second).
3. P2 bench variants in the worktree, bench files only (`framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp`, `framework/bench/grpc/cpp/client/bench_cpp_client.cpp`), each behind a command-line flag defaulting to off so the default path is byte-for-byte unchanged in behaviour: S1, S2, S3, C1, C2 exactly as plan §6 P2's table describes. Keep each variant small; no Core, binding or Framework runtime change. A runner xp1/scripts/run-p2.sh that measures each variant once at the 22k equal-rate pacing with the xio thread CPU capture (µs/msg per I/O thread) plus the P1 perf stat counts, and an analyzer row per variant.
4. A dry check that does not measure: `bash -n` every script, `python3 -m py_compile` every analyzer, and run the analyzer on a fake input directory you create under xp1/dry/ to show the output tables.

## Rules
- Read only the worktree /root/worktree/zlink-v21-s033/xio (origin/main 39bbfe470d plus the xio bench pacing change), /root/jobs/fx/cx/v21/s033/xio/ and /root/jobs/fx/cx/v21/s033/xp1/. Write only in /root/jobs/fx/cx/v21/s033/xp1/ and the two bench files in the worktree.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status. No GitHub operations. Do not read Core C headers (core/include). Do not modify core/ or bindings/.
- Run `bash scripts/format/format.sh --check cpp` in the worktree and fix what it reports in the bench files.
- Time box about 3 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/xp1/prep.md (Korean, plain sentences)
(1) scripts and what each step runs, with the exact perf commands; (2) symbol check per uprobe (found, mangled name, or missing/inlined); (3) P2 variants with file:line and the flag names; (4) dry-check output; (5) the exact command the execution job should run and the expected wall time. Save the bench diff as xp1/bench.diff.
