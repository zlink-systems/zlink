Model gpt-6-luna, effort high: implement an approved bench-driver spec for one language with fixed scope; no product code. Earlier job briefs are history, not instructions.

# Task: #4190 — Java and Kotlin (framework/bench/grpc/java: client, shared, run_local.sh, run_local_kotlin.sh and the Kotlin aux client) — 8 dedicated platform submit threads

Folder JOB=/root/jobs/fx/cx/v21/s033/s4190j. Worktree /root/worktree/zlink-v21-s033/s4190j on branch bench/4190-s4190j (= origin/main, includes #4191).

## Spec (merged in #4191, 0f2f937e7f): framework/bench/grpc/README.ko.md §10.2–§10.3 (English pair README.en.md). Read it first.
- `request-backpressure` and `send-saturation` run `submit_concurrency` (default 8) logical streams; `request-serial` stays 1 stream, in-flight 1.
- **One stream = one dedicated submit thread** (C++, .NET, Java, Kotlin; C reference too). That thread owns its stream's submission and its own counters/in-flight/latency samples; merge the per-stream aggregates once at phase end. Completion callbacks run on the language's own completion resource — the bench does NOT move them to the stream thread. Node: all streams on the one event loop.
- Binding (raw zlink), Framework and gRPC rows of the same language use the same thread layout (§7.2, §10.4 last paragraph: gRPC A runs one unary stub loop per stream).
- Trigger field is `submitConcurrency` (was `sendConcurrency`); result metadata records stream count and per-stream in-flight for every pattern (§4/§10.3). `requestWindow` stays as is.
- Shared runner edit (every job makes exactly this identical edit so branches merge cleanly): framework/bench/grpc/runner_common.sh:10 `SEND_CONCURRENCY=8` → `SUBMIT_CONCURRENCY=8`, and :193 `\"sendConcurrency\":${SEND_CONCURRENCY}` → `\"submitConcurrency\":${SUBMIT_CONCURRENCY}`. Change nothing else in runner_common.sh.

## Known single-submitter assumptions to fix (from /root/jobs/fx/cx/v21/s033/b4190/report.md — read it): shared sequence counters and non-atomic metrics, error summaries written from several callbacks, submission gates. Sequence numbers must stay unique across streams (e.g. per-stream prefix or one atomic counter); reply validation stays per-sequence.

## Verify
- The language's existing bench unit tests pass (update tests that name sendConcurrency).
- Smoke: run the language's run_local.sh for request-backpressure 1024 and send-saturation 1024 (binding and Framework rows, warmup 5 s, active 5 s is enough) and request-serial 1024 once; completed counts > 0, errors 0, results.json metadata shows submitConcurrency 8 / streams 8 (1 for serial). While a backpressure cell runs, record the source process thread list (/proc/<pid>/task/*/comm) showing the 8 submit threads (Node: not applicable). Smoke runs take `flock -o /root/jobs/fx/cx/measure.lock` for the run only.
- No performance numbers are judged in this job; the supervisor measures after merge.
- Remove committed stray run outputs under framework/bench/grpc/<lang>/framework/bench/grpc/log/ if your language has them (they are accidental results.json copies) — say what you removed.

## Report: 결과, 판정 위치(전/후: 제출 thread 수 결정·sequence·집계 병합 위치 file:line), 바꾼 것(파일:줄), 검증(명령·로그·thread 목록), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"), 남은 문제. Commit on your branch, do not push.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C/C++ compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, build dirs inside your worktree; C++ binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>. Core: release package /root/.cache/zlink/core-release/1.24.0/linux-x64 (ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=that). If a build gets SIGSTOPped by the measurement guard, wait.
- Change only files of your language (plus the one runner_common.sh edit). Keep raw outputs in your JOB folder. Report in Korean, short. Remove build outputs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/s4190j/report.md
