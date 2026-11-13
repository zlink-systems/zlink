Model gpt-6-luna, effort high: a bounded A/B measurement of one branch against main with fixed cells and commands; no design or code change.

# Task: #1639 receive-role handoff (C++) — why target CPU per message is higher on request-backpressure, with named worker threads

Folder JOB=/root/jobs/fx/cx/v21/s033/m1639idle3. Earlier attempt: /root/jobs/fx/cx/v21/s033/m1639idle2 (read report.md, raw/, measure/attempt4/ and reuse its build.sh, run-locked.sh and analysis scripts). That run found request-backpressure 1024 target CPU µs/msg higher on the PR in all 3 rounds (+8%, +17%, +12%; raw/measurement-summary.csv) but could not attribute it: offload worker threads had no OS name, so "zlink-mesh-app" counted 0 threads.

Two worktrees, already created, read-only for you (build outputs only, no commits or tracked-file changes):
- BASE=/root/worktree/zlink-v21-s033/m1639-base3 (detached a2342b92b6 = main c52c06f9ae + only the thread-naming commit, so both sides name their worker threads)
- PR=/root/worktree/zlink-v21-s033/trole-pr (branch perf/1639-cpp-receive-role-handoff, head 1ddf44954b = PR #4302, merges the same main c52c06f9ae)

## Do
1. Build the C++ Framework bench for both trees exactly as m1639idle2 did, but Core prefix P=/root/.cache/zlink/core-release/1.25.0/linux-x64 (matches bindings/cpp/VERSION). Release, ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON (never /root/.cache/zlink/zlink-cpp/<ver>), builds only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dirs inside each worktree. Prove each bench binary links its own tree's framework (link line + sha256) as m1639idle2 did. Chain: build rc 0 && binary check && measure; never measure a stale binary.
2. Measurement conditions as m1639idle2: `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE only around the runs; cpu0-15 governor performance inside the lock, powersave restored by trap; source CPUs 0–7, target 8–15; uptime/load, free memory, scaling_cur_freq 0-15 before and after every run.
3. Cell: Framework C++ request-backpressure 1024 only. Warmup 30 s, active 5 s, 5 rounds, BASE/PR alternating inside each round (swap the order every round), same session.
4. Per run, per process (source and target): msg/s; total CPU µs/msg from before/after /proc/<pid>/task snapshots summed over the TID union (user+system); AND the same split by thread name (/proc/<pid>/task/*/comm) — at least: zlink-mesh-app workers, Core I/O threads, other — giving µs/msg per group, plus the number of live threads per group at start and end of the active window, and voluntary/nonvoluntary context switches per message per group (/proc/<pid>/task/*/status).
5. Report: table BASE vs PR (5-round median and range) for msg/s, total CPU µs/msg source/target, and the per-group µs/msg and context switches/msg for target. State in one plain sentence which thread group carries the CPU difference (or that the difference is within round-to-round spread), with the raw paths.
6. Report sections: 결과, 측정 조건, 검증(명령·로그·원본 경로), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Keep every raw file in JOB; never delete raw outputs. Every claim with a raw-file path. Report in Korean, short. Remove build outputs at the end; leave both worktrees.

## Output: /root/jobs/fx/cx/v21/s033/m1639idle3/report.md

## Waiting is expected
measure.gate is closed by the gate keeper whenever any measurement waits for or holds the exclusive measure.lock, and reopens 180 s after it ends (log: /root/jobs/fx/cx/quiet/gate-keeper.log). A closed gate is normal: keep waiting and do not stop because of it. Never touch the gate or lock files. Report only after the numbers exist, or after 3 hours of waiting with the gate-keeper log as evidence.
