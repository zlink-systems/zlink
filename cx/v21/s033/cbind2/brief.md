Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: C++ bench process ended by signal 11 after its HTTP listener failed to bind — reproduce, find the owner and the cause with file:line, fix it if it is in production code.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock. Do not touch other worktrees or their build outputs (read other code with git show only).
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all of it, incl. BUILDS BETWEEN HOLDS).

## Observation (c4json job, raw kept)
/root/jobs/fx/cx/v21/s033/c4json/raw/baseline-v2-L3-request-serial-1024/: source.log ends with
`zlink application failed: HTTP listener bind/listen failed: http://127.0.0.1:58242: Address already in use ...` then `C++ bench HTTP host exited with rc=1`; startup-events.json shows the source process status -11 (signal 11) instead of an orderly non-zero exit. Ladder scratch commit 62e766f88d (exp/3868-cpp-ladder-next); bench host code framework/bench/grpc/cpp/common/bench_stats_server.hpp:311-375 (stats_http_server_t::start/stop) on that commit and on origin/main.
Expected behaviour: a failed HTTP bind ends the process with a clear error and a non-zero exit code, no signal.

## Steps
1. Worktree: `git -C /root/zlink worktree add /root/worktree/zlink-v21-s033/cbind2 -b fix/cpp-http-bind-failure-exit origin/main`. Build the C++ bench client (bench_cpp_client) the way lad/c4json did, only via /root/jobs/fx/cx/cpp-build-slot.sh (Release + RelWithDebInfo-style -g is allowed, USE_BINDINGS_SOURCE=ON, Core prefix 1.25.0, -j3, ≤25 GB, build dir inside this worktree). Check rc 0 and binary newer than sources.
2. Reproduce: occupy the source's stats port first (e.g. a python listener), start the client with the same arguments as c4json's runner (see c4json/tools), pinned with taskset -c 6-7,14-15. Repeat ≥10 times; record exit status each time. Get a full `gdb -batch -ex run -ex 'thread apply all bt'` stack (or core file) for a signal-11 run. Keep all output under /root/jobs/fx/cx/v21/s033/cbind2/raw.
3. Determine the owner precisely: bench code (what does the client do after start() returns false?), Framework C++ app_t run/stop/destroy path after a failed listener (framework/languages/cpp/framework), or binding/Core. Quote file:line for each frame you rely on and trace the call path from the bind failure to the faulting access. Check whether a production C++ Framework app (not bench) hits the same path when http().listen fails to bind — write a minimal unit/integration test that starts an app_t with an already-bound HTTP port and asserts run() returns non-zero and the process/app destroys cleanly; it must fail before the fix (or show it passes, if the defect is bench-only).
4. If production: fix in framework/languages/cpp with that test (fails before, passes after), C++ format check (`scripts/format/format.sh --check cpp`), and the framework tests covering the touched files. If bench-only: fix the bench and say so. Local commit only.

## Output
/root/jobs/fx/cx/v21/s033/cbind2/report.md (Korean, short): reproduction rate, stack with file:line, owner, cause, fix and before/after test result, commit hash. Time cap 1.5 h.
