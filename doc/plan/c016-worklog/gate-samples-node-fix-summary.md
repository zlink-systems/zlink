# Node sample follow-up: ZoneWorld and SupportChat

## Result

SupportChat now passes twice in Chromium. ZoneWorld can resolve `vite` after the
client-local dependency install, but its rerun exposes a second checkout/content
blocker: the Vite inputs `game.html` and `ops.html` are absent. The effective Node
sample result is therefore 6/7.

## ZoneWorld dependency environment

No dependency-install command for the shared browser client was present in the
ZoneWorld runner, sample README, or the read-only Node sample guide. The client
does have its own `package.json` and `package-lock.json`; `vite` is declared at
`framework/languages/shared_sample/zoneworld/client/package.json:24`.

Following the requested fallback, this environment step was run without changing
either manifest:

```bash
cd framework/languages/shared_sample/zoneworld/client
npm install
```

It installed 126 packages. The audit reported three existing dependency findings
(one moderate and two high); no `npm audit fix` was run.

The ZoneWorld rerun advanced past `ERR_MODULE_NOT_FOUND`, completed its server-side
scenario verdicts, and invoked Vite 7.3.6. It then failed after 123 seconds with:

```text
Could not resolve entry module "game.html".
```

`framework/languages/shared_sample/zoneworld/client/vite.config.ts:9-10` requires
`game.html` and `ops.html`, but neither file exists in the working tree or in the
repository index/history. This is still classification **D (environment/repository
content)**, not a protocol failure. Full log:
`zlink-work/c016/logs/gate-samples-node-ZoneWorld-fix.log`.

## SupportChat root cause and classification

The timed-out operation was the first post-reconnect `ConversationIdleNotify` wait
created at
`framework/languages/node/samples/SupportChat.Ts/Client/supportchat-client-scenario.ts:201-203`.
The original preserved browser log shows that the same conversation's idle
notifications had already reached the old customer and agent connections at lines
17-18, before the scenario closed and recreated those clients. A `waitFor` observes
future messages; the reconnected clients therefore could not receive the already
delivered notification and expired at browser-log lines 101-108.

The cause was a timeout-budget race in the sample:

- the server declared a 3,000 ms idle timeout and a 1,000 ms close grace period;
- before arming the post-reconnect waits, the browser scenario created and joined
  three conversations, exercised several 250 ms negative checks, and reconnected
  both participants;
- depending on scheduling, conversation 1 became idle on the old sessions or
  reached Closed before the resume request;
- after increasing the lifecycle interval, the simultaneously armed close waiter
  also needed to exceed `idleTimeout + closeGraceTimeout`, rather than the former
  10,000 ms connector default.

The existing OTel flow files under
`/dev/shm/zlink-tmp-node/zlink-supportchat.ts-OapA8T/logs/flow/` show successful
relay transitions through `received`, `admitted`, `dispatched`, and `replied`.
`ZLINK_DEBUG_FRAMEWORK_RELOCATION=1` produced no relocation failure. Searches of
the runner, browser, and flow logs found no `BACKPRESSURED` or `EAGAIN`. Recorded
flow errors were the scenario's intentional negative cases, including unauthenticated
requests, wrong-conversation metadata, and duplicate close.

Classification: **B (sample lifecycle timeout budget)**. It is not **E**: no
REQUEST submit returned D-B85 `BACKPRESSURED`/`EAGAIN`, and there is no unsettled
binding-port request at a resubmit site.

## Minimal fix and regression

- `framework/languages/node/samples/SupportChat.Ts/Server/Configuration/sample-names.ts:13-14`
  sets idle to 10,000 ms and close grace to 2,000 ms. This prevents the first
  conversation from expiring during browser setup and matches the established
  2-second close grace used by the other implementations.
- `framework/languages/node/samples/SupportChat.Ts/Client/main.ts:27` sets the
  browser connector wait budget to 20,000 ms, covering the 12-second idle-plus-close
  lifecycle with margin.
- `framework/languages/node/test/contract/sample-supportchat-message-semantics-gate.test.js:34-47`
  guards those lifecycle budgets. No assertion was removed or weakened.

Focused regression:

```text
node --test test/contract/sample-supportchat-message-semantics-gate.test.js
2 tests passed
```

SupportChat reruns, both with `TMPDIR=/dev/shm/zlink-tmp-node`, the Node gate flock,
and `ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`:

| Attempt | Result | Duration | Log |
|---|---|---:|---|
| 1 | PASS | 26 s | `zlink-work/c016/logs/gate-samples-node-SupportChat-fix-final-1.log` |
| 2 | PASS | 27 s | `zlink-work/c016/logs/gate-samples-node-SupportChat-fix-final-2.log` |

Both runs observed the intended idle, resume message, second idle, and automatic
close notifications before `supportchat=completed` and `supportchat-placement=completed`.

## Final seven-sample table

The five unchanged passes are carried forward from
`gate-samples-java-node-summary.md`; SupportChat and ZoneWorld are the reruns from
this follow-up.

| Node sample | Result | Evidence |
|---|---|---|
| TicTacToe | PASS | Prior individual gate, 12.322 s |
| Bingo | PASS | Prior individual gate, 10.815 s |
| DeliveryDispatch | PASS | Prior individual gate, 8.402 s |
| SupportChat | PASS | Fixed browser scenario, 2/2 (26 s, 27 s) |
| GameQuest | PASS | Prior individual gate, 6.204 s |
| ShoppingMall | PASS | Prior individual gate, 5.686 s |
| ZoneWorld | BLOCKED / FAIL (D) | `vite` installed; missing `game.html`/`ops.html` input, 123 s |

## BLOCKERS

- Node remains 6/7. ZoneWorld's shared browser source is incomplete in this
  checkout/repository state: `vite.config.ts:9-10` names two HTML entry files that
  are absent. Adding or restoring those files is a separate source-content change,
  not an npm installation step, so it was not invented in this environment-only
  follow-up.
- The client-local npm audit reports one moderate and two high findings. They did
  not block installation or the Vite launch and were not auto-fixed.
