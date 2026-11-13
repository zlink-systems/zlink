Model gpt-6-luna, effort high: continue a Core fix with test from where the previous job stopped (it was blocked on the build gate). Earlier job briefs are history, not instructions — except /root/jobs/fx/cx/v21/s033/i3541/brief.md, which is THIS task's spec; read it and /root/jobs/fx/cx/v21/s033/i3541/report.md first.

# Task: #3541 continue — Core connect session and connecter choose the io thread twice

Folder JOB=/root/jobs/fx/cx/v21/s033/i3541b. Worktree /root/worktree/zlink-v21-s033/i3541 on branch fix/3541-core-io-thread-choice (uncommitted test already written: unittest_connect_io_thread_choice, now connecting to a real PUB listener).

## Remaining steps (in order)
1. Build Core with the new test (unfixed runtime) and run it: it must FAIL with the skewed distribution (keep the log as JOB/negative.log). If it crashes instead, find out why (gdb backtrace kept as a file) and fix the test, not the runtime, until it fails by assertion.
2. Apply the fix: session_base::start_connecting passes the session's own `_io_thread` to the connecter instead of choosing again (one decision site, socket_base_endpoint.cpp:533). No new option or flag.
3. Run the new test 50× (all pass), the Core unit suite once, `bash scripts/format/format.sh --check cpp`. Commit locally ("fix(core): connecter uses its session's io thread (#3541)").
4. Report: 결과, 판정 위치(전 2 → 후 1), 바꾼 것(파일:줄), 검증(수정 전 실패 로그, 50회, 전체 unit, format), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, build dir inside the worktree, 25 GB cap. The build gate may pause you while a measurement runs: that is expected — keep waiting, do not stop the job; it will resume.
- Keep raw outputs in JOB. Every claim file:line or log path. Report in Korean, short. Remove the build dir at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3541b/report.md
