# Task: Java Framework — submit service replies directly through the binding (no port state-lane turn), with tests (product change for a PR)

## Situation
Measurement study /root/jobs/fx/cx/v21/s033/jsend/report.md (read its "시험한 첫 변경" part and /root/jobs/fx/cx/v21/s033/jsend/direct.diff) found that `ZLinkJavaRawServicePort.replyMessages` runs every reply inside `inStateLane(...)` (awaitTurn) plus `ensureOwnedOnLane(router)`. Replies from many handler turns queue on that lane, reach the socket spread out, and leave in more, smaller TCP packets. Submitting directly via `router.reply(target, token).message(...).submit()` raised throughput 27.7% in the study copy.
The binding already owns what the lane re-checks: bindings/doc/spec/README.ko.md "Multipart 제출" (concurrent independent record submit on one handle follows Core same-handle concurrency; no socket-local gate), bindings/doc/spec/java/README.ko.md Raw reply one-shot, reply token validity, close gate.

Worktree: /root/worktree/zlink-jdr, branch perf/1639-java-direct-reply on latest main 475305bebc. Only this file is expected to change in main code:
framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java

## Do
1. Change `replyMessages` to call the binding reply directly (keep the argument validation and `Message.closeAll` in finally; drop `inStateLane` and `ensureOwnedOnLane` for this call only). Do NOT change tcpNoDelay or any other socket option. Leave other lane uses unchanged.
2. Before changing, list every caller of `reply`/`replyMessages` and every place that catches or classifies the exceptions they throw (IllegalStateException "service port is closed", IllegalArgumentException "router is not owned"). Report, with file:line, what a late reply (after runtime close or port close) produced before and what it produces after. The rule: a late reply must end with the binding/Core error, the Framework must not add its own judgement, and the outcome kind seen by callers (failure vs silent drop, which observer/metric fires) must stay the same. If any caller depends on the old exception type, adjust that caller so the result kind stays the same — do not reintroduce a Framework-side gate.
3. Check what port close does to routers (close order vs a reply submit in flight on another thread) and state, with file:line in the Java binding, why a submit racing with socket close is safe (binding close gate) — read bindings/java source, do not modify it.
4. Tests (Java Framework test tree, follow nearby test style):
   a. runtime close and port close overlapping with replies: result kind is the same as before (late reply → binding/Core error path, no hang, no leaked Message).
   b. several handlers replying concurrently from different threads: each reply reaches its own request (token) — no cross delivery, all complete.
   c. run the full Java Framework test suite (the commands CONTRIBUTING/scripts use for framework Java, including integrationTest) through `/root/jobs/fx/cx/measure-shared.sh <cmd>`; and `scripts/dev/format.sh --check java` (or the repo's format command for Java).
5. Report the decision count: reply submission judged in 2 places before (Framework port lane + owner check, binding/Core) → 1 after.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate checks or dead helpers this change leaves (e.g. now-unused code), remove them in this change; no new queue, lock, cache, flag or state.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/jdr/report.md (Korean, plain sentences)
Order: (1) change and decision count; (2) late-reply result kind before/after with file:line; (3) close race safety with binding file:line; (4) tests added and full test/format results.
