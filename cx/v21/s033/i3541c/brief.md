Model gpt-6-luna, effort high: finish the #3541 Core fix; the test needs one correction found by diagnosis. Earlier job briefs are history, not instructions — except /root/jobs/fx/cx/v21/s033/i3541/brief.md (this task's spec).

# Task: #3541 finish — make the test measure only connect-session placement

Folder JOB=/root/jobs/fx/cx/v21/s033/i3541c. Worktree /root/worktree/zlink-v21-s033/i3541 on branch fix/3541-core-io-thread-choice (uncommitted: the fix in core/src/runtime/core/session_base.cpp:852 and the test core/tests/unittest/unittest_connect_io_thread_choice.cpp). Read /root/jobs/fx/cx/v21/s033/i3541b/ (negative.log, fixed-50.log) and the diagnosis /root/jobs/fx/cx/v21/s033/i3541b/j3541d-report.md first.

## Diagnosis (PM, read the report for file:line)
- After the fix, TCP and TLS connect sessions both choose an io thread once (socket_base_endpoint.cpp:533, session_base.cpp:853). No TCP-only extra choice exists.
- The test puts the PUB listener in the SAME ctx as the subscribers; accepted sessions consume the same round-robin counter (asio_tcp_listener.cpp:248, asio_tls_listener.cpp:301, ctx_io_thread_registry.cpp:99). When connects and accepts interleave, connect sessions advance two slots at a time → the 13/37/14/36 pair pattern in fixed-50.log. The product fix is fine; the test measures a subset of a shared sequence.

## Do (in order)
1. Change the test so only connect sessions use the measured ctx's counter: put the publisher (listener) in a separate ctx. Do not change the product fix. No new option or flag.
2. Negative check with the corrected test: temporarily revert ONLY session_base.cpp to HEAD, build, run — it must FAIL by assertion (two choices per connect); keep JOB/negative.log. Restore the fix.
3. With the fix: new test 50× (all pass, keep the log), Core unit suite once, `bash scripts/format/format.sh --check cpp`. Commit locally ("fix(core): connecter uses its session's io thread (#3541)").
4. Report: 결과, 판정 위치(전 2 → 후 1), 바꾼 것(파일:줄), 검증(수정 전 실패 로그, 50회, 전체 unit, format), 리팩토링 점검 한 줄, 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, build dir inside the worktree (core/build exists; reuse it), 25 GB cap. The build gate may pause you while a measurement runs: expected — keep waiting, do not stop the job.
- Keep raw outputs in JOB. Every claim file:line or log path. Report in Korean, short. Remove the build dir at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3541c/report.md
