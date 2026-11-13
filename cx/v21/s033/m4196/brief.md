Model gpt-6-luna, effort high: a bounded A/B measurement of one PR against main with fixed cells and commands; no design or code change.

# Task: PR #4196 (.NET binding: per-call native reservation CAS+decrement on SocketHandle._state) — send hot-path cost, main vs PR

Folder JOB=/root/jobs/fx/cx/v21/s033/m4196. Two worktrees, already created: BASE=/root/worktree/zlink-v21-s033/m4196-base (origin/main, the PR's merge base) and PR=/root/worktree/d4196-pr (PR head b807c1e748, read-only for you: do not commit or change files there except build outputs).

PR change summary: every native call (send/request/reply submit, receive, completion_recv in the drain) now does Interlocked.CompareExchange(+1) and Interlocked.Decrement on one long in SocketHandle (bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs), and submit paths read it once more (EnsureSubmitOpen). The submit threads and the completion drain thread share that cache line.

## Do
1. Build the .NET bench for both trees (framework/bench/grpc/dotnet; find its build/run scripts and the earlier .NET measurement commands in /root/jobs/fx/cx/v21/s033/*/report.md, e.g. grep -l dotnet). Both builds must use their own tree's binding source (check the built Zlink.dll path/timestamp belongs to that tree and say how you checked).
2. Under `flock -o /root/jobs/fx/cx/measure.lock` (measurement only), condition B (source CPUs 0–7, target CPUs 8–15), warmup 30 s, active 5 s, 3 rounds alternating base/PR within each round, same session: binding (zlink-dotnet raw) send-saturation 1024, send-saturation 4096, request-backpressure 1024; Framework .NET send-saturation 1024. Record msg/s and per-process CPU per message from before/after /proc/<pid>/task snapshots summed over the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py).
3. Report: table per cell — base vs PR msg/s (3 rounds, median), PR/base ratio and per-round range, CPU µs/msg source and target, and whether the difference exceeds the round-to-round spread. Plain verdict: measurable regression or not.
4. Report sections: 결과, 검증(명령·로그 경로·원본 경로), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Core: Core 1.24.0 release package P=/root/.cache/zlink/core-release/1.24.0/linux-x64 (ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=$P) for both trees.
- Keep every raw file in JOB; never delete raw outputs. Every claim with a raw-file path. Report in Korean, short. Remove build outputs at the end; leave both worktrees.

## Output: /root/jobs/fx/cx/v21/s033/m4196/report.md
