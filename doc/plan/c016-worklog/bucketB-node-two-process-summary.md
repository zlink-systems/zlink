# node bucket-B — `user-spot-native-two-process` RequestError

## Result

The failure is a Node Framework sequencing defect in the remote User Spot
operation requester, not a Core or binding result-mapping defect. The requester
advertised terminal replay, but gave the first raw request the entire remaining
end-to-end deadline. A lost reply therefore completed as `TimedOut` only after
the replay loop had no time left to resend the same operation.

The fix is in
`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4008-4026`.
Each raw request attempt now receives half of the remaining deadline, leaving a
bounded opportunity to replay the same operation ID and recover the target's
already-computed terminal. No public timeout or assertion was increased.

Classification: **Framework sequencing / response-loss recovery**. No files
under `bindings/node/**` or `core/**` were changed.

## Captured terminal and end-to-end trace

The exact test command was initially run three times without source changes.
All three passed (774.8 ms, 878.3 ms, and 894.5 ms), so the gate failure is not
deterministic in isolation.

The original gate log did not serialize `RequestError.result`, `code`, or
`nativeErrno`. A temporary diagnostic probe added those standard properties,
enabled `ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`, and installed the Node OTel log
provider with `messageFlow('detailed')`. To reproduce the same response-loss
boundary deterministically, a temporary test-only branch dropped the target's
first raw reply after it was constructed. The probe was removed after capture.

Captured sequence:

1. Source command: `create` (command 47 User Spot create).
2. Target: `m6b-target-*`; it executed the create and constructed a one-part
   reply to source RID `m6b-source-*`.
3. The first reply was deliberately dropped at the raw reply boundary.
4. Source Core request completed after the five-second budget with
   `request_result=101` (`RequestResult.TimedOut`).
5. Child error properties were `result=101`, `code=101`, and `nativeErrno=0`.
   `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:149-163`
   creates `RequestError` directly from `completion.requestResult`.
   `NativeCompletion` has `requestResult` but no request errno
   (`completion_owner.ts:32-40`), and `RequestError` therefore keeps its default
   errno of zero.
6. OTel `messageFlow` records were `[]`. This is expected: command 47 is
   RouteMesh infrastructure/control-plane traffic, outside application-dispatch
   message-flow coverage. The target-side raw reply trace supplied the missing
   control-plane evidence.

The controlled pre-fix failure took 6.36 s and matched the original gate's
6.07 s signature. After the fix, the same forced first-reply loss passed in
3.90 s: the second request reused the same encoded operation, and the target
replayed its retained terminal instead of executing the lifecycle again.

This also distinguishes the case from an unknown RID. The Core contract at
`core/doc/spec/core/socket/07-router.ko.md:205-222` requires an unknown routing
ID to fail at submit with `ZLINK_SUBMIT_NOT_FOUND+ENOENT` and no completion.
Here submit succeeded and a REQUEST completion later reported timeout.

## Regression test

`framework/languages/node/test/m6b/m6b-user-spot-terminal-replay.contract.ts:68-126`
adds `command 48 reserves deadline for terminal replay after a transport
timeout`. The fake raw transport consumes its first attempt timeout and loses
that response. The test verifies that:

- the first attempt receives less than the full 500 ms end-to-end budget;
- a second request is issued;
- the encoded request, including operation ID and close fence, is byte-identical;
- the replayed success terminal reaches the caller.

Before the fix, the first attempt received all 500 ms and the test failed before
a second request. The production loop is shared by User Spot create, User Spot
close, and Actor create, so command 48 pins the common behavior without adding
a test-only production API.

## Commands and results

All commands ran from `framework/languages/node` unless stated otherwise.

```bash
for run in 1 2 3; do
  TMPDIR=/dev/shm/zlink-tmp-node ZLINK_DEBUG_FRAMEWORK_RELOCATION=1 \
    node --test --test-force-exit --test-timeout=600000 \
    test/contract/user-spot-native-two-process.test.js
done
```

Pre-fix natural reproduction: **3/3 passed**. This established the gate failure
as intermittent rather than deterministic in isolation.

```bash
TMPDIR=/dev/shm/zlink-tmp-node \
ZLINK_DEBUG_FRAMEWORK_RELOCATION=1 \
ZLINK_TEST_DROP_FIRST_RAW_REPLY=1 \
ZLINK_TEST_TRACE_CHILD_STDERR=1 \
node --test --test-force-exit --test-timeout=600000 \
  test/contract/user-spot-native-two-process.test.js
```

Temporary pre-fix response-loss reproduction: **failed 0/1**, 6.36 s;
`create`, `result=101`, `code=101`, `nativeErrno=0`, `flow=[]`.

The same command after the sequencing fix: **passed 1/1**, 3.90 s, with the
target trace confirming that its first one-part reply was dropped. All temporary
reply-drop, stderr forwarding, error-property, and OTel probes were then removed.

```bash
npm run verify:m6b-runtime
```

The new regression passed. Overall result: **109 passed, 1 failed**. The failure
is unrelated and reproducible alone:
`test/m6b/m6b-runtime.contract.ts:5533-5542` expected
`sendActorBoundSession(...) == SubmitResult.Ok`, but observed
`SubmitResult.InvalidState` (`8`). It does not exercise User Spot operation
request/replay and was not changed.

```bash
node --test \
  --test-name-pattern='command 48 reserves deadline for terminal replay after a transport timeout' \
  build/m6b-runtime/languages/node/test/m6b/m6b-runtime.contract.js
```

Regression-only result: **1 passed, 0 failed**, 295.7 ms.

```bash
npm run build
```

Final Framework build: **passed**.

```bash
for run in 1 2 3; do
  TMPDIR=/dev/shm/zlink-tmp-node \
    node --test --test-force-exit --test-timeout=600000 \
    test/contract/user-spot-native-two-process.test.js
done
```

Post-fix two-process result: **3/3 passed** (842.4 ms, 975.9 ms, 833.0 ms).

```bash
TMPDIR=/dev/shm/zlink-tmp-node \
node --test --test-force-exit --test-timeout=600000 test/contract/*.test.js
```

Whole contract result: **1,534 passed, 4 failed**. The target test passed as
test 1538 in 1.40 s. Remaining failures are pre-existing environment/sample
items: one sample runner failure because Playwright Chromium is not installed,
and three ZoneWorld checks because expected `samples/ZoneWorld/dist/**` modules
are absent.

```bash
git diff --check
```

Result: **passed**.

## BLOCKERS

- No blocker remains for `user-spot-native-two-process`.
- The full M6B verification still has the unrelated
  `sendActorBoundSession` `InvalidState` failure at
  `test/m6b/m6b-runtime.contract.ts:5533-5542`.
- The whole contract suite cannot be fully green in this environment until
  Playwright Chromium and the expected ZoneWorld emitted modules are available.
