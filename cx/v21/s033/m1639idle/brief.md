Model gpt-6-luna, effort high: a bounded A/B measurement of one branch against main with fixed cells and commands; no design or code change.

# Task: #1639 receive-role handoff (C++) — submit-path cost of the idle-worker check, main vs PR branch

Folder JOB=/root/jobs/fx/cx/v21/s033/m1639idle. Two worktrees, already created: BASE=/root/worktree/zlink-v21-s033/m1639-base (detached fd0e665655 = the main the PR merged) and PR=/root/worktree/zlink-v21-s033/trole-pr (branch perf/1639-cpp-receive-role-handoff, head 725ddf3085; read-only for you: do not commit or change tracked files there, build outputs only).

PR change summary: framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.cpp replaces the idle-worker counter with a per-worker assignment slot and an intrusive idle list. Every submit (try_submit_internal, try_submit_application) now calls idle_workers_exceed_locked(_queue.size()) under the executor mutex, which walks the idle list and stops at the (queue length + 1)th idle worker. The mesh application executor (mesh_node_host_service.cpp, "zlink-mesh-app") has min = registrations+1 and max = registrations+max(2, CPUs) workers with a 100 ms idle timeout. The question: does this cost anything measurable on the submit path?

## Do
1. Build the C++ Framework bench (framework/bench/grpc/cpp) for both trees. Find the build/run commands in earlier C++ measurement reports (grep -l "zlink-framework-cpp" /root/jobs/fx/cx/v21/s033/*/report.md; e.g. trole, pool2, m4196 for the snapshot method). Both: Release, binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON; never /root/.cache/zlink/zlink-cpp/<ver>), Core release prefix P=/root/.cache/zlink/core-release/1.24.0/linux-x64, builds through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dirs inside each worktree. Prove each bench binary links its own tree's framework (path + timestamp or sha256) and say how you checked.
2. Measurement conditions (approved by the PM, 2026-11-11): hold `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for the measurement only (builds closed); set cpu0-15 scaling_governor to performance only inside the lock and restore powersave on exit (trap; template /root/jobs/fx/cx/v21/s033/f4162/aa-clean.sh); condition B: source process CPUs 0–7, target 8–15; log uptime/load, free memory and cpu frequencies (/sys/.../scaling_cur_freq for 0-15) before and after every run. Do not hold the lock longer than the runs themselves.
3. Cells, Framework C++ only: send-saturation 1024 and request-backpressure 1024. Warmup 30 s, active 5 s, 3 rounds, alternating base/PR inside each round, same session.
4. Per run record: msg/s; per-process CPU µs/msg from before/after /proc/<pid>/task snapshots summed over the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py); and the number of live "zlink-mesh-app" threads on each process at the start and end of the active window (thread names from /proc/<pid>/task/*/comm) — this shows the worker count the walk actually sees.
5. Report: table per cell — base vs PR msg/s (3 rounds, median), PR/base ratio and per-round range, CPU µs/msg source and target, live mesh-app worker count, and whether the difference exceeds round-to-round spread. Plain verdict: measurable submit-path regression or not.
6. Report sections: 결과, 측정 조건(governor·고정·잠금·load·주파수 기록 경로), 검증(명령·로그 경로·원본 경로), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Keep every raw file in JOB; never delete raw outputs. Every claim with a raw-file path. Report in Korean, short. Remove build outputs at the end; leave both worktrees.

## Output: /root/jobs/fx/cx/v21/s033/m1639idle/report.md
