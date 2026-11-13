# Task: Java direct reply — fix port/runtime close when a direct reply is in flight (review finding)

## Situation
Read /root/jobs/fx/cx/v21/s033/jdr/report.md and `git diff` in /root/worktree/zlink-jdr (branch perf/1639-java-direct-reply). The change moved `ZLinkJavaRawServicePort.replyMessages` out of the state lane.

Review finding: before, `close()` claimed close in the state lane, so no reply submission could be inside a native call when `entry.getKey().close()` ran. Now a reply can be inside the native submit on another thread. Core close is a fail-fast gate (core/doc/spec/core/socket/README.ko.md §2: an admitted API running on the same handle makes close return EBUSY). Java binding `NativeSocketRuntime.closeInternal` (bindings/java/.../NativeSocketRuntime.java ~615) throws `ZlinkCloseException` for a non-OK close, and `CompletionOwner.withNativeCall` checks `closed` without a lock, so this race is reachable. Then `ZLinkJavaRawServicePort.close()` catches it, resets closeAttempt and rethrows, so runtime close can fail under load. The new race tests pass only because the window is small (closeResult.get would fail if it hit).

## Do
1. Confirm or refute this with file:line (binding close path, the BUSY mapping in ZlinkException, Core gate). If you can, reproduce it deterministically (e.g. many reply threads in a loop while close runs, repeated) and show the failure on the current diff.
2. Fix it without bringing back a Framework-side gate on the reply hot path and without a new lock, queue, flag or counter. First look for a binding feature that already owns this (a close that waits for in-flight calls, a documented BUSY contract for owners — check bindings/doc/spec/java/README.ko.md and bindings/doc/spec/README.ko.md close/lifecycle sections, and how the .NET binding's socket DisposeAsync handles EBUSY, framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkRawRouterServicePort.cs:169-192 and the .NET binding source it calls). Use what the binding defines. If the binding contract says the owner retries close on BUSY, do exactly that in port close (bounded by the existing close attempt; replies that start after close is accepted get ESHUTDOWN from Core). State which and why, with spec lines.
3. Make the race tests able to hit the window (several reply threads submitting repeatedly while close runs, repeated N times) and assert close completes without exception every time, every Message is consumed, and replies end as success or binding/Core closed failure.
4. Re-run the full Java Framework :zlink-framework-core:test and :integrationTest through `/root/jobs/fx/cx/measure-shared.sh`, samples classes, and `scripts/format/format.sh --check java`.
Also: in the test file replace the fully qualified java.util.* / systems.zlink.contracts.errors.* names with imports to match the file's style.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: say whether the fix adds or removes any decision point; no new queue, lock, cache, flag or state.
- Time box about 2 hours.

## Output: append a section "2차 수정" to /root/jobs/fx/cx/v21/s033/jdr2/report.md (Korean, plain sentences): finding confirmed/refuted with file:line, reproduction result before fix, the fix and its binding basis, test results.
