Model gpt-6-luna, effort high: reproduce a test hang under load and capture stacks; read-only diagnosis, no code change.

# Task: reproduce issue #4094 — C++ test_cpp_framework_actor_late_result hangs under load — and capture where it waits

## Situation
In the pre-PR run of PR #4073 (Java-only change, C++ = main 6b24e01426), ctest reported `test_cpp_framework_actor_late_result` Timeout 30.07 s. Last lines: `[ RUN ] local_actor_receiver_t.LocalRegistryTerminalsReachPreparedReceiverOnce` with no OK; earlier `submitted_actor_receiver_t.ShutdownWhileAdmissionPendingDeliversOnce` printed RUN and a stats line but no OK line before the next suite started (check whether gtest output order explains this). Full log: /root/jobs/fx/pre-pr/fix/4060-java-binding-submit-close/cpp-ctest.log (around line 600-740).
The test binary from that run is preserved: /root/jobs/fx/cx/v21/s033/clate/test_cpp_framework_actor_late_result (built at commit in binary-commit.txt; links /root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so.0). Source: /root/worktree/zlink-4060-binding-close/framework/languages/cpp (read only; test file under tests/Zlink.Framework.UnitTests).

## Do
1. Read the test file and the code paths of LocalRegistryTerminalsReachPreparedReceiverOnce and ShutdownWhileAdmissionPendingDeliversOnce (file:line): what each waits on and which thread should release it.
2. Reproduce: run the binary with `--gtest_filter=` for the whole binary and for each of those two tests, repeated (`--gtest_repeat`, or a loop) under load: `taskset -c 8-11` for the test (4 CPUs) and `stress-ng --cpu 8 --taskset 8-15` alongside, both inside `/root/jobs/fx/cx/measure-shared.sh`; stop stress-ng with SIGTERM. Use a per-run timeout of 30 s (`timeout -s TERM 30`). Run until 5 hangs or 2000 runs, whichever first; record the hang rate. Also run without load (1000 runs) for comparison.
3. On a hang, before the timeout fires, capture all thread stacks with `gdb -p <pid> -batch -ex 'thread apply all bt'` (save to clate/stacks-N.txt), then let it be terminated with SIGTERM. Never SIGKILL.
4. From the stacks and code, state what the stuck thread waits for, who should have signalled it, and why that signal is missed or late (file:line). If it is a test-side wait without a deadline, or a product-side ordering defect, say which. Do not fix.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033; write only in clate/.
- Do not modify any repository or the binary. No builds. No git operations except diff/status/log/show. No spec edits; no GitHub operations. Do not read core/include. Do not run scripts/dev/internal-docs.sh.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short; test output to files.
- 리팩토링 점검: note duplicate waits/handoffs you see on the stuck path with file:line.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/clate/report.md (Korean, plain sentences): (1) test intent and wait points with file:line; (2) reproduction rates with and without load and commands; (3) stacks summary; (4) cause hypothesis with evidence, classified as test-side or product-side.
