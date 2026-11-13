Model gpt-6.1-sol, effort high: concurrency design for a reproduced contract defect (mailbox owner claim vs execution gate); design only — the supervisor approves before implementation. Earlier job briefs are history, not instructions.

# Task: #1639 same-owner defect — decision-owner table and fix design (no product change)

Folder JOB=/root/jobs/fx/cx/v21/s033/sameowner2. Worktree /root/worktree/zlink-v21-s033/sameowner (detached at 76a4d78506; contains the uncommitted repro test from the previous job — keep it, you may finish it). Read first: /root/jobs/fx/cx/v21/s033/sameowner/report.md, repro.log, repro.diff; /root/jobs/fx/cx/v21/s033/trole/design.md (approved receive-role handoff A — a parallel job is implementing it in mesh_node_host_service.cpp/offload_executor.cpp; your design must not conflict with it; say where they touch).

## Facts
- Public C++ API repro: with handler A of a gate-less ClientServer channel blocked, request B to the SAME channel starts 0/20; to a different channel 20/20 (repro.log:1).
- Spec framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md §2 (:97-105, changed by #3891 bf24a5a02c): Node and ChannelName handlers have no gate; turns are per record; the next record's pipeline may run on another worker before the previous handler suspends/completes. Same gate's application turns never run concurrently (:106).
- C++ refuses a claim while `active_messages != 0` in service_mailbox.cpp:212-217 (try_claim_owner) and :235-237 (claim_owner_locked); a claim stays active until its last settlement (:432-452). Callers: channel_host_service.cpp:151,389; client_server_location_runtime.cpp:1629; raw_stateful_dispatch.cpp:1646; public_host_runtime.cpp:8549; service_mailbox.cpp:191 (and mesh node host).

## Do
1. Decision-owner table for "may another worker take the next record of an owner while that owner has an active claim": spec clause; every place that decides it now (file:line, including the gated paths — Spot/Actor/raw stateful — that RELY on the mailbox check for gate exclusion, if any); the single owner after the fix; counts before/after. If the gated paths depend on the mailbox check for correctness, the fix must keep exclusion for them without a new flag per call site — e.g. the owner's gate property known once at owner registration, or gated callers owning exclusion through their gate. Pick the design with the fewest decision sites; no new flags/counters unless one is removed per added one.
2. Per-record vs per-claim: with the fix, how many records does one claim take for a gate-less owner (budget), and does FIFO dequeue order still hold (§2 says dequeue FIFO, entry order not fixed). Settlement accounting (:432-452) after the change.
3. Tests: the repro becomes a regression test (fails now, passes after); finish the gated-owner control (the previous job's actor readiness assertion failed at :463 — find why: location store/lease setup) so it asserts B does NOT start for a gated owner. Run what you can on current main (build exactly as below) to show: gate-less same owner 0/20 now; gated control holds. Do not change product code.
4. .NET and Java: point to the file:line that decides the same thing (sameowner report lists candidates) and say whether they pass the same scenario by source reading; flag if either has the same defect.
5. Report: 결과, 결정 소유 표(전/후 수), 설계안(파일:줄별 변경), trole2와의 접점, 시험안, 검증(명령·로그), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Build (exact), from framework/languages/cpp
P=/root/.cache/zlink/core-release/1.24.0/linux-x64
cmake --preset linux-ninja-release -B <worktree>/build-so -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=$P -Dzlink_DIR=$P/lib/cmake/zlink -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build <dir> --target test_cpp_framework_client_server_runtime --parallel 3
Run with LD_LIBRARY_PATH=$P/lib. (Worktree is at 76a4d78506; if its bindings need Core 1.23, use /root/.cache/zlink/core/1.23.0/linux-x64 instead and say so.)

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), build dir inside the worktree, 25 GB cap; never /root/.cache/zlink/zlink-cpp/<ver>. If SIGSTOPped by the measurement guard, wait.
- Keep raw outputs in JOB; save the test source diff as JOB/repro2.diff. Every claim file:line. Report in Korean, short. Remove the build dir at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/sameowner2/report.md
