Model gpt-6-luna, effort high: evidence work — map symbolized allocation stacks from a Windows CI log and a Linux run onto C++ Framework source; no design choice is delegated.

# Task: #4143 — attribute every Windows-only allocation in two request allocation tests, Windows vs Linux, with file:line

Earlier job briefs are history, not instructions.

## Situation
- Test file: framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp in the worktree /root/worktree/zlink-v21-s033/w4143 (branch test/4143-windows-alloc-symbols; main plus symbol names in Windows allocation stacks).
- Windows CI log with symbolized stacks: /root/jobs/fx/cx/v21/s033/w4143c/win-run42640081756.log (win-x64 MSVC Release). Two failures:
  - `LocalActorRequestCompletionRegistrationDoesNotExceedBaselineHeapCalls`: `EXPECT_LE(sample.total, 8u)`, Windows 10.
  - `NodeRequestUsesOnlyItsPreparedReceiverAboveTheTransport` at :740: actual 9, expected encode+target_owner+receiver baselines = 4 on Windows.
- Supervisor's first reading (candidates, not conclusions): in the local Actor test the extra events are MSVC `std::deque` storage — `state_lane_t::_mailbox` (`std::deque<mailbox_item_t>`, src/runtime/execution/state_lane.hpp:534) and `offload_executor_t::_queue` (`std::queue<work_item_t>`, src/runtime/dispatch/offload_executor.hpp:119) allocate a map (64 B) and an element block on push. In the node test there are `std::map<std::string,std::string>` node copies (route_client_t::request_to_node, client_call_codec_t::create_envelope, envelope_header_t constructor) and a 640 B `std::function` heap block in envelope_codec_t::prepare_service_payload (envelope_codec.cpp:579). Confirm or refute each with the source and the Linux run.

## Do
1. From the Windows log, list every allocation event of both failing samples (and of each baseline sample the node test subtracts): size, the first Framework frame with file:line, and the owning object.
2. Build and run the same test binary on Linux (Release, same commit) with the allocation stack dump enabled for both tests (the test prints stacks on failure; if it passes on Linux, run the same sampling helper and print the events — a local test-only print in this job folder's copy is fine, do not commit). Produce the same list for Linux.
3. Pair Windows and Linux events. For each Windows-only or Linux-only event, state why (standard library container growth policy, std::function small-buffer size, map copy happening inside vs outside the counted window, a different code path), with the standard library behaviour cited from the library source that ships with the toolchain (libstdc++ headers on this machine; for MSVC cite the frame names in the log and say what can and cannot be confirmed from here).
4. Separate the node test's 9 vs 4 into: events that also exist on Linux but fall inside the baseline samples there, and events that are new on Windows. Say whether the test's baseline method itself is platform-sensitive.
5. Decision-owner table: for each extra event, the owner (file:line), whether it is product cost on every call (e.g. one allocation per state-lane post on MSVC) or a one-time growth, and the smallest product change that removes it (e.g. a fixed ring buffer instead of std::deque in state_lane/offload_executor, avoiding a header map copy, a capture that fits std::function's buffer), or "test limit only" with the reason. Do not implement the product change.
6. Prevention: propose how a PR that adds or changes an allocation-count limit gets a Windows observation before merge (which CI job and trigger, and what the PR check would show). One paragraph.

## Rules
- Do NOT build, test or take any lock until the file /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (a Core release gate is measuring). Until then, read the log and the source only.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` called bare (no outer flock), linux-ninja-release preset, no -g, build only the one test target; keep w4143c/ plus build dirs under 25 GB. Tests through /root/jobs/fx/cx/measure-shared.sh. SIGTERM/SIGINT only; no background jobs with &.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only the worktree and /root/jobs/fx/cx/v21/s033/w4143c/. Write only in /root/jobs/fx/cx/v21/s033/w4143c/ (build dirs there too). Do not modify the worktree.
- No git operations except git diff, git status, git log, git show. No commit; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- Keep every raw output (Linux event lists, logs) under w4143c/raw/; never delete them.
- 리팩토링 점검: list duplicated copies (header maps, envelope headers) and wrappers on the request submit path that these events reveal, with file:line.
- Time box about 4 hours. Keep your own output short.

## Output: /root/jobs/fx/cx/v21/s033/w4143c/report.md (Korean, plain sentences)
(1) Windows event list per sample; (2) Linux event list per sample; (3) pairing and reasons; (4) node test 9 vs 4 breakdown; (5) decision-owner table with product change or test-limit reason; (6) prevention proposal; (7) 리팩토링 점검.
