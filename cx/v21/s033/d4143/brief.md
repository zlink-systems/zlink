Model gpt-6-luna, effort high: design-only — decision-owner table and Linux allocation counts for a known, attributed allocation set; no product change yet (supervisor approves the design first). Earlier job briefs are history, not instructions.

# Task: #4143 — design the removal of avoidable request-path allocations (C++ Framework), Linux counts, no implementation

Folder JOB=/root/jobs/fx/cx/v21/s033/d4143. Worktree /root/worktree/zlink-v21-s033/d4143 (detached at origin/main). Read first: /root/jobs/fx/cx/v21/s033/w4143c/report.md (every Windows allocation attributed with file:line) and the last two comments on issue #4143 (copied below).

PM's direction (approved):
- node test (`NodeRequestUsesOnlyItsPreparedReceiverAboveTheTransport`, test_cpp_framework_request_call_allocations.cpp): the 5 extra allocations are per-call product cost — metadata copied by value at channel.hpp:920 (1) and `envelope_header_t` moved by value 4 times on the #4014 service-encoder path (channel_runtime.cpp:1880 → client_call_codec.cpp:90 → envelope_codec.cpp:579/589). MSVC std::map allocates a new head node on every move. Remove the by-value copy/moves (helps both platforms). The test's reference measures the eager encoder (`encode_payload_parts`), not the real service-encoder path: change the reference to the real path.
- registration test (`LocalActorRequestCompletionRegistrationDoesNotExceedBaselineHeapCalls`): extra 4 are first-enqueue MSVC deque map/block allocations (state_lane.cpp:129, offload_executor.cpp:105) on a cold host. Make the measurement use the same path under the same initial conditions (one warm-up may not be enough on MSVC: explain why it is or isn't, from the deque growth rule). No Windows-only limit.

## Do
1. For each allocation above, the decision-owner table: the decision (who owns the header/metadata map during submit; when container storage is created), the spec clause or header doc that owns it, every place that decides it now (file:line), and the single owner after the change; decision-site counts before/after. No new flags, counters, or parameters unless one is removed for each.
2. The concrete change per site (signature before/after, e.g. take `const&`/rvalue and build the header in place; capture by pointer to an owned object; avoid moving the map through the std::function closure), and why each stays correct (lifetime of the referenced map through the deferred encoder — trace when the encoder runs relative to the submit frame).
3. Linux numbers: build only the target test_cpp_framework_request_call_allocations (Release preset, binding from source) and record each sample's total and sizes on current main, so the PR can show Linux before/after. Do not edit code to do this.
4. The test changes: the new node reference (exactly which call it measures) and the registration warm-up/initial condition, with the test that fails before the product fix on Linux if any (say plainly if Linux cannot show the failure, and which Linux assertion will prove the removed moves — e.g. a smaller expected count).
5. Report sections: 결과, 판정 위치(전/후 수), 변경안(파일:줄별), 검증(명령·로그 경로·수치), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Build (exact)
cmake --preset linux-ninja-release -B <worktree>/build-rel -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64 -Dzlink_DIR=/root/.cache/zlink/core/1.23.0/linux-x64/lib/cmake/zlink -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
(run from framework/languages/cpp; if the preset path differs, read CMakePresets.json and say what you used). Compile through /root/jobs/fx/cx/cpp-build-slot.sh cmake --build ... -j3 --target test_cpp_framework_request_call_allocations.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C++ compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dir inside your worktree; binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>.
- Do NOT change product or test code in this job. Keep every raw output in JOB. Every claim file:line or raw-file path. Report in Korean, short. Remove the build dir at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/d4143/report.md
