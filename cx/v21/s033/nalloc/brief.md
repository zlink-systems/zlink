Model gpt-6-luna, effort high: the issue pins the files, the binding feature to reuse and the done condition; the work is a bounded Node change plus tests and a same-session measurement.

# Task: Node Framework encodes outgoing frames directly into Message.allocate (#4053)

## Situation
Issue #4053 (text below). The Node Framework builds the outgoing application frame in a JS Buffer, then the binding copies it into native storage with Message.from. That is one extra payload-size copy per direction.
- Path: framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts:498 appendRequestParts → operation.message(Uint8Array) → bindings/node/src/zlink/contracts/messaging/received_operations.ts:25 Message.from (message.ts:105).
- Frame writer: framework/languages/node/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts:256 writeMultipartPayload, :265 result.set.
- Existing binding features to reuse (do not add a new binding API or a new copy path): Message.allocate(size) returns writable native storage (bindings/node/src/zlink/contracts/messaging/message.ts:118-123); operation.message(Message) uses the Message without copying (received_operations.ts:25, socket_operation_builders.ts:42).
- Baseline structure: .NET writes the protobuf body straight into the native Message span (framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:100).
- Cross-language report: /root/jobs/fx/cx/v21/s033/xlang/report.md (Node body passes 4/2 vs .NET 3/2). A separate study changed the Node receive side in another worktree; do not touch the receive path.
Worktree: /root/worktree/zlink-4053-node-alloc = latest main + CPU-pin patch.

## Do
1. Trace every outgoing frame writer that ends in operation.message(Uint8Array) for send, request and reply (file:line). Say who owns the Message after submit, on submit failure and on close, from the binding contract (bindings/doc/spec, rg Message ownership / consume).
2. Change the Framework to allocate the final frame with Message.allocate(length), write the multipart frame into its data() once, and pass the Message. Close it on every path where the binding does not take it. Keep the protobuf body write where it is unless it can write into the same storage without a second path.
3. Tests: a contract test that the outgoing path does not call Message.from (or equivalent observable proof: copy count 3), a test that a failed submit and a close after allocate release the Message, plus the existing suites. Run `npm test` in full for the Node framework packages and record the log.
4. Measure, binding in the same session: unpaced RUNS=5 send-saturation, request-backpressure and request-serial 1KiB, main vs change.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree.
- Builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; other measurements are queued, keep waiting), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work (same check/encode/copy/handoff twice) you see, with file:line, and whether your change removed it; no new queue, lock, cache, flag or state.
- Report "over budget by X µs" per message (budget = binding µs / 0.6), never as a percentage. Do not suggest lowering the 0.6 target.
- Run `scripts/format/format.sh --check node` (or the repo's Node format check) and `git diff --check` at the end.
- Time box about 4 hours; time spent waiting for the measurement lock does not count.

## Output: /root/jobs/fx/cx/v21/s033/nalloc/report.md (Korean, plain sentences): (1) writer sites and Message ownership; (2) change, decision-site count before/after; (3) tests with log paths; (4) RUNS=5 table with Framework÷binding and over budget by X µs. Save framework/ diff as nalloc/change.diff and bench edits separately.
