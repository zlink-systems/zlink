# Node SupportChat aggregate follow-up (round 2)

## Result

The recurring aggregate-only SupportChat timeout was a client scenario ordering
defect, not a dropped push to a resumed session. The scenario let the first
conversation's idle clock continue from an earlier chat while it created a third
conversation, ran negative checks, reconnected two streams, rejoined three rooms,
and explicitly closed another room. It also registered the second idle/close
waiters only after the resume message that started the next idle interval.

The fix sends one ordinary typed chat message, with its peer push waiter already
armed, immediately before reconnect. This establishes a committed idle baseline.
After reconnect, the first idle waiters are armed before the unrelated explicit
close, and the second idle/close waiters are armed before the resume request.
No timeout value was changed.

Final result: the four-sample aggregate order passed 3/3, SupportChat alone passed
2/2, the focused contract gate passed, and the complete seven-sample runner exited
0.

## Preserved failure evidence

The original aggregate failure was copied without modification to:

- `zlink-work/c016/artifacts/supportchat-aggregate-fail-YSzaGY/`
- `zlink-work/c016/artifacts/supportchat-aggregate-fail-3dvy1t/`

Each directory contains `logs/browser-client.log` and the OTel flow files under
`logs/flow/`. The corresponding aggregate logs are
`zlink-work/c016/logs/gate-final-samples-node-aggregate-2.log` and
`zlink-work/c016/logs/gate-node-supportchat-round2-aggregate-pre-1.log`.

In `YSzaGY`, the stream/session mapping is established by the authentication
flows: old agent/customer streams are `00000001`/`00000002`; reconnected customer
and agent streams are `00000006`/`00000007`. The browser labels map those resumed
streams to client-5 and client-4 respectively.

## Failure timeline and expired waits

| Time (UTC) | Scenario point | Server flow | Gateway/browser evidence | Finding |
|---|---|---|---|---|
| 23:06:06.145-.155 | Explicit cid2 close and duplicate-close check finish; the old source then arms the first cid1 idle waits at lines 201-202. | cid2 close completes. | client-4 receives cid2 `ConversationClosedNotify`. | The first cid1 waits are on the resumed clients, but are armed after reconnect setup and the explicit-close work. |
| 23:06:15.900-.907 | First cid1 idle wait is pending. | Four `ConversationIdleNotify` sends are recorded (cid3 and cid1 participants), all `sent` successfully. | cid1 reaches client-5 and client-4; cid3 reaches client-6 and client-4. | Bound-session push to both resumed cid1 sessions works. The notification was not sent to the old sessions. |
| 23:06:15.911-.916 | The customer resume request runs; its peer push waiter was armed first. | `SendChatMessageReq` is received/admitted/dispatched/replied, and `ChatMessageNotify` is sent. | client-4 receives the chat push and client-5 receives the response. | Resume over the new bindings works. This rules out a general post-reconnect delivery gap. |
| after 23:06:15.916 | Old source lines 212-215 arm the second cid1 idle pair and close pair, after the resume action that started the next idle interval. | The support log later records cid1 `WaitingForClose` and `Closed`, but the preserved OTel file has no second cid1 `ConversationIdleNotify` or `ConversationClosedNotify` `sent` event before shutdown. | No corresponding client-5/client-4 inbound message appears; heartbeats continue until timeout. | There is no server-emitted notification that the gateway dropped. The observable gap begins before bound-session delivery, while the scenario has overlapping, independently started conversation lifecycles. |

The three browser exceptions represent three rejection surfaces, not exactly three
underlying wait registrations:

1. `Promise.all([secondIdleCustomer, secondIdleAgent])` at old line 216 rejects
   once after the waits registered at old lines 212 and 213 expire.
2. The already-created `idleClosedCustomer` wait at old line 214 rejects without
   being awaited after the idle aggregate fails.
3. The already-created `idleClosedAgent` wait at old line 215 rejects for the same
   reason.

Thus all four second-lifecycle registrations were left unsatisfied; JavaScript
reports one error from the idle `Promise.all` and two additional unhandled close
rejections. The first idle waits at old lines 201-202 did not expire in these
round-2 aggregate failures.

## Cause and contract

Cause: `framework/languages/node/samples/SupportChat.Ts/Client/supportchat-client-scenario.ts`
previously performed reconnect and rejoin at old lines 161-192, explicit cid2
close at old lines 194-199, and only then armed cid1's first idle waits at old
lines 201-202. It armed the second idle/close waits at old lines 212-215 only after
the resume request at old lines 206-210. No application message reset cid1's idle
baseline immediately before reconnect.

This violates the shared sample contract in
`framework/doc/framework/common/sample/supportchat/README.ko.md:361-365`: when
multi-process preparation or reconnect validation can consume the domain idle
deadline, the runner sends a normal typed `SendChatMessageReq`, verifies its
response and new `MessageSeq`, and then verifies idle/grace with a separate bounded
wait. The state-machine clause at lines 356-359 requires idle to notify both
participants, resume within grace to return to Active, and otherwise close with a
terminal notification. Reconnect must preserve state and use the new binding
(`README.ko.md:49,60,370-373`).

The C++ client already follows the required order:
`framework/languages/cpp/samples/SupportChat/Client/supportchat_client_scenario.hpp:212-224`
sends and verifies a real pre-reconnect chat, and lines 281-295 arm both first-idle
waits before explicit close. The .NET, Java, and Kotlin clients likewise arm idle
and close waits together immediately after reconnect and before explicit close.

## Fix and regression

- `framework/languages/node/samples/SupportChat.Ts/Client/supportchat-client-scenario.ts:161-172`
  now arms a peer chat wait, sends `Still looking into it.`, and verifies sequence
  3 and Active before reconnect.
- The reconnect assertions at lines 191-205 verify that the resumed sessions read
  the committed sequence 3 state rather than hoping to catch an in-flight notify.
- Lines 207-210 arm both first-idle waits before explicit cid2 close.
- Lines 219-224 arm the resumed peer push plus both second-idle and both auto-close
  waits before the resume request; the resume notification is sequence 4.
- `framework/languages/node/test/contract/sample-supportchat-message-semantics-gate.test.js:49-66`
  guards all three arm-before-act relationships.

No file under `packages/framework/**` was changed because the failing run proves
that the resumed bindings deliver the first idle and resume pushes, while no later
server `sent` event exists for a gateway to drop.

## Determinism table

| Phase | Invocation | Result | Duration | Evidence |
|---|---|---:|---:|---|
| Baseline | Inherited aggregate run | FAIL, SupportChat wait timeout | not re-timed | `gate-final-samples-node-aggregate-2.log` |
| Baseline | Aggregate reproduction 1 | FAIL, same three-error surface | 62.475 s | `gate-node-supportchat-round2-aggregate-pre-1.log` |
| Baseline | SupportChat alone (round 1) | PASS 2/2 | 26 s, 27 s | `gate-samples-node-SupportChat-fix-final-{1,2}.log` |
| Fixed | SupportChat alone green 1 | PASS | 30.338 s | `gate-node-supportchat-round2-alone-post-retry-1.log` |
| Fixed | SupportChat alone green 2 | PASS | 27.324 s | `gate-node-supportchat-round2-alone-post-2.log` |
| Fixed | Aggregate green 1 | PASS | 59.307 s | `gate-node-supportchat-round2-aggregate-post-1.log` |
| Fixed | Aggregate green 2 | PASS | 59.504 s | `gate-node-supportchat-round2-aggregate-post-2-retry.log` |
| Fixed | Aggregate green 3 | PASS | 53.870 s | `gate-node-supportchat-round2-aggregate-post-3-retry.log` |
| Fixed | Full seven-sample runner | PASS, exit 0 | 237.507 s | `gate-node-supportchat-round2-full-7.log` |

Two non-SupportChat lifecycle attempts were excluded from the required green
counts but retained as evidence:

- The first post-fix standalone invocation stopped during agent re-authentication
  with `supportchat.api` channel result 103 after 4.929 s. Its flow records a
  channel shutdown, before any idle wait. The next two standalone runs passed.
- One aggregate invocation stopped at Bingo after 23.241 s, before SupportChat
  started. The next two aggregate invocations passed, completing three evaluated
  SupportChat aggregate runs.

## Verification

All commands ran from `framework/languages/node` with
`TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` unset, and
`flock -w7200 /tmp/zlink-node-gate.lock` around the invocation.

```text
node --test test/contract/sample-supportchat-message-semantics-gate.test.js
3 tests passed

bash samples/run_samples.sh SupportChat.Ts
2/2 evaluated runs passed

bash samples/run_samples.sh TicTacToe.Ts Bingo.Ts DeliveryDispatch.Ts SupportChat.Ts
3/3 evaluated runs passed

bash samples/run_samples.sh
TicTacToe, Bingo, DeliveryDispatch, SupportChat, GameQuest, ShoppingMall, ZoneWorld passed
exit 0
```

## BLOCKERS

None for the requested Node SupportChat fix or seven-sample gate. The two
unrelated one-off failures above remain recorded but did not reproduce on the
required evaluated runs.
