# diag: cross-language `user-spot-join-node-dotnet` — canonical actorJoin(28) never observed

## Symptom

`ZLINK_CPP_CROSS_LANGUAGE_STAGE=user-spot-join-node-dotnet` (Node source host → .NET
target host) failed deterministically:

```
timed out waiting for canonical actorJoin(28) in <run>/node-user-spot-join-source.events.flow
```

The source flow showed `UserSpotDiscoveryProbeReq` request/reply, `BeginUserSpotJoinReq`
replied, then a `$zlink.actor.source-leave.v1` channel send — but never the
`canonical actorJoin: wire_command=28 canonical=true packet=ZLinkFrameworkActorJoinRequest`
line the stage waits for.

## What the kept run dir showed

With `ZLINK_CPP_CROSS_KEEP_RUN_DIR=1` the two hosts' event files disagree with the
stage verdict: **the Join actually succeeded end to end.**

`dotnet-user-spot-target.events`:

```
user-spot-created|spot=cross-lang-user-spot|...|state=Created
user-spot-source-peer-ready|ready=true|peers=1
user-spot-admission|accepted=true|actor=cross-lang-user-spot-actor|spot=cross-lang-user-spot
user-spot-joined|actor=cross-lang-user-spot-actor|...
user-spot-probe-served|nodeRid=...
user-spot-probe|nodeRid=<target>|targetRid=<target>|actor=cross-lang-user-spot-actor|stateVersion=7
```

`node-user-spot-join-source.events` likewise ends with
`user-spot-join-request-reply|accepted=true`. So RouteMesh admission, the canonical
command-28 ingress on the .NET side, the User-Spot admission callback, the joined
lifecycle and the target-owner Actor probe all worked. The .NET D-132 admission change
(`5b749308e1`) is **not** implicated — nothing was fenced, dropped or re-generated.

Only the *evidence* was missing.

## Cause

`framework/languages/node/cross-language/user_spot_join_host.js:91`
(`installCanonicalWireProbe`) monkey-patched `ServiceStatefulRuntime.submitRequest`
and logged the canonical line when `operationKind === 'actorJoin'`.

Node commit `a58c6b6b81` (2026-09-06 13:20, "remote Actor Join goes through the durable
operation sender") moved the **remote** Actor Join off that method:

`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4344-4357`

```ts
if (targetNodeRid === this.nodeRid) {
  this.submitRequest(pending, targetNodeRid, parts, ..., 'actorJoin', actor);
  return;
}
void this.requestDurableOperation(targetNodeRid, parts, pending.id, 'actorJoin', deadlineMs)
```

Since that commit `submitRequest` sees only *local-target* Actor Joins. This stage is
always cross-node, so the probe was silently detached from the only path it needed to
observe — a false red, and the stage's canonical-vs-private-fallback discrimination was
gone as well. (The 10:30 pass the brief cites predates `a58c6b6b81` at 13:20.)

## Fix

Attach the probe to the **single service-transport seam** that both senders use rather
than to one sender method, and key it on the wire command byte instead of the internal
`operationKind` string:

- `framework/languages/node/cross-language/user_spot_join_host.js`
  - new `ACTOR_JOIN_WIRE_COMMAND = 28` constant (header offset 3).
  - `installCanonicalWireProbe` now wraps `stateful.raw.requestService(targetNodeRid,
    parts, timeoutMs)` and decodes `decodeActorJoin28(parts)` whenever `parts[0][3] === 28`.
    The plain sender (`service-stateful-runtime.ts:4281`) and the durable lifecycle
    sender (`:4106`) both terminate there, so the probe cannot detach again when the
    runtime changes which sender owns a Join.
  - the guard now fails fast on a missing service transport instead of a missing
    `submitRequest`.

The emitted flow line, the `canonical=true/false` discrimination, the
`--force-private-join` hook and the multi-attempt hooks are unchanged.

No framework/runtime source was touched: `a58c6b6b81` is the correct parity behaviour
(Join is a durable lifecycle operation), and nothing about the observed wire traffic
changed — only where the harness listens.

Rule count: probe attachment points 1 → 1 (sender method → transport seam); no new
fence, timer, retry or sleep.

## Regression test

`framework/languages/node/test/contract/actor-join-transport-seam.test.js` (new, 4 cases:
canonical × entry-spot/user-spot). It drives `ServiceStatefulRuntime` with a fake service
transport whose `reserveLocalIngress`/`sendService` throw, and asserts every **remote**
Actor Join reaches `requestService` exactly once with `parts[0][3] === 28`, the pending
correlation, the right `entry` flag, and a decodable application packet name. If a future
change routes a remote Join off `requestService` again, this fails deterministically
instead of the cross-language stage going red for a missing log line.

## Gates

| gate | result |
| --- | --- |
| `node --test test/contract/actor-join-transport-seam.test.js` | 4/4 pass |
| `user-spot-join-node-dotnet` before fix | reproduced red twice (x15 rerun + x16 repro1) |
| `user-spot-join-node-dotnet` after fix | 5/5 pass |
| `user-spot-join-dotnet-node` (reverse) | pass |
| cpp cross-language all-stage (`ZLINK_CPP_CROSS_LANGUAGE_STAGE` unset) | `cross-language smoke result=passed` — 32/32 stages, including `Node source -> Java target` and `Node source -> C++ target` User-Spot Join, which share this probe |
| dotnet unit split A (`FullyQualifiedName!~CanonicalActorJoinIngressReplyTests`, `--blame-hang 10m`) | Passed 2009 / Failed 0 |
| dotnet unit split B (`CanonicalActorJoinIngressReplyTests`) | Passed 16 / Failed 0 |
| node runtime gate (`npm run verify:p0`: build + typecheck + lint + runtime tests) | announced=1701 completed=1701, exit 0 |

Nothing left red. `MeshNodeShutdownSealTests` is inside split A and stayed green; no
`framework/languages/dotnet/**` file was modified by this fix.

Logs: `zlink-work/gates/x16/{repro1,repro2,stage5,all-stage,dotnet-units,node-gate}.log`.
