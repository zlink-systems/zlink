Model gpt-6-luna, effort high: reproduce through the public API, no product fix. Earlier job briefs are history, not instructions.

# Task: same Node/Channel owner — does a blocked handler A stop record B? (#1639)

Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch.

## Background
C++ mailbox refuses a new claim while the owner has active messages (framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:212-217 and 235-237 at 76a4d78506), so while handler A for one Node/Channel owner blocks synchronously, record B of the same owner may not run even if workers are free. Spec: framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md §2 (:97-108) and the change in PR #3891 ("let several workers drain a gate-less owner"; read it with `git log -1 --stat <merge commit>` / `git show` locally — find it with `git log --oneline --grep "#3891"`). Design note: /root/jobs/fx/cx/v21/s033/trole/design.md §8 T05 and /root/jobs/fx/cx/v21/s033/p2/report.md.

## Do
1. Work in /root/worktree/zlink-v21-s033/sameowner (already created, detached at 76a4d78506).
2. Write a public-API repro test (C++ Framework public API only, the way existing integration/contract tests host a Node or Channel and send to it): same owner, no gate; handler A blocks on a latch the test controls; send B; with bounded waits check whether B's handler starts before A is released. Also the control cases: different owner B, and a gated owner (B must NOT start). Run each 20×.
3. Run the same scenario against .NET and Java if their test suites can do it cheaply (read-only check is fine: point to their file:line that lets B run, or show they also block).
4. Classify: if B does not run, it is classification B (existing contract defect); write the decision-owner table (who decides same-owner concurrency now, file:line, and the single owner a fix would use) but do NOT fix it.
5. Build exactly like this (the previous attempt failed because it configured without the vcpkg toolchain/manifest and could not find opentelemetry-cpp), from framework/languages/cpp:
   cmake --preset linux-ninja-release -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64 -Dzlink_DIR=/root/.cache/zlink/core/1.23.0/linux-x64/lib/cmake/zlink -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
   /root/jobs/fx/cx/cpp-build-slot.sh cmake --build build/linux-ninja-release --target <your test target> --parallel 3
   (this exact sequence built test_cpp_framework_operation_registry on 2026-11-11; vcpkg installs into the build dir's vcpkg_installed). Never use /root/.cache/zlink/zlink-cpp/<ver>. 25 GB cap. Remove the build dir at the end (leave the worktree for the supervisor); keep the test source as repro.diff in the job folder.

## Rules
- No GitHub, no sub-agents or other AI tools. No benchmarks. Every claim file:line. Report in Korean, short.

## Output: /root/jobs/fx/cx/v21/s033/sameowner/report.md
