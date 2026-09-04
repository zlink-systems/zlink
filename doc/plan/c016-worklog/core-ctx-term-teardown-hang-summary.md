# ctx_term teardown hang — dump analysis and Core repro (2026-09-05)

**Classification: framework/test teardown ordering (leaked DEALER), not a Core defect.**
`zlink_ctx_term()` blocks because one fixed-RID DEALER created by the test helper was never
closed. Core behaves as specified; a public-C-API repro shows no hang when every socket is
closed and shows the documented block-until-close when one is not.

## 1. Dump findings

Dump: `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/TestResults/a9f8d99f-fccf-464e-80db-530d1ba6587a/dotnet_92047_20260905T051922_hangdump.dmp`
(test host pid 92047, `RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`).
Tools: `dotnet-dump analyze` (managed), `gdb -batch -c <dmp> /usr/bin/dotnet` (native; `libzlink.so`
from `bin/Debug/net8.0/runtimes/linux-x64/native` carried full symbols, so native frames resolved).

### Thread table

| LWP | Role | Where it waits |
|---|---|---|
| 92063 (0x1679f) | test thread, `ConnectedRuntime.DisposeAsync` @ CanonicalActorJoinIngressReplyTests.cs:1303 → `Systems.Zlink.Context.Dispose` → `zlink_ctx_term` | `ctx_t::terminate` (core/src/runtime/core/ctx.cpp:198) → `wait_for_reaper_done` (ctx_termination.cpp:106) → `mailbox_t::recv(-1)` → `signaler_t::wait` → `poll(-1)`; waiting for the reaper's `done` command |
| 93753 | Core reaper poller (`asio_poller_t::loop`, asio_poller.cpp:460) | idle timed wait (no reap work: the last socket never entered reaping) |
| 93754 | Core `control_runtime_t::loop` (control_runtime.cpp:254) | idle cond wait |
| 93755–93758 | Core io threads (`asio_poller_t::loop`) | idle `epoll_wait(100 ms)` |
| all other threads | .NET runtime / vstest / xunit infra | no Core frames |

No managed thread is inside `zlink_poller_wait`, `zlink_close`, or any other Core entry point;
only the ctx_term thread is in Core.

### Native context state (gdb)

- `ctx_t 0x7cf880011f40`: `_terminating = true`, `_socket_registry._sockets` = **1 socket**: `0x7cf888003440`.
- That socket: `options.type = 5` (`ZLINK_SOCKET_DEALER`), `routing_id_size = 50` (fixed RID),
  `linger = -1` (default), `_ctx_terminated = true` (ctx_term's `stop` reached it),
  **`own_t::_terminating = false`, `_term_acks = 0`**, public handle `0x7cf888017250` state `0`
  (no `closing_bit`). I.e. `zlink_close()` was never called on it; the reaper never got a `reap`
  command, so `done` can never be sent.

### Managed handle audit (dotnet-dump)

- 12 `Systems.Zlink.Context` objects on the heap; only `7cf9719b80a0` has a live `_handle`
  (`0x7cf880011f40`, the terminating context).
- 28 `SocketHandle` objects: 27 have `_handle = 0` (closed). Exactly one, `7cf9719e5270`, has
  `_handle = 0x7CF888017250`, `_context = 7cf9719b80a0` — the unclosed DEALER above.
  `gcroot 7cf9719e5270` → **0 roots**: the managed `DealerSocket` wrapping it was dropped without
  `Dispose`. (The binding's `SocketHandle` has no finalizer/SafeHandle, so GC gives no safety net —
  consistent with the explicit-close contract.)
- All 15 `Poller` and 15 `SocketMonitor` objects have `_handle = 0` (closed). The mesh node's
  RouterSocket (`7cf9719afcb8`, rooted from `ZLinkManagedMeshNode.DisposeCoreAsync` @ :2977) is
  closed (`_handle = 0`).
- Exception object present in the dump: `System.TimeoutException("Route admission reply was not
  received.")` thrown by `ConnectedRuntime.SendHelloUntilAdmittedAsync` ← `HandoverAsync` ←
  `RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`.

### Leak path (file:line)

`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs`
- :1150 `HandoverAsync` — `var replacement = Context.CreateDealerSocket();` (:1151), connect with the fixed RID.
- :1165 `using var admission = await SendHelloUntilAdmittedAsync(replacement, SourceEndpoint);`
  throws `TimeoutException` (deadline 2 s, :1213–:1241) **before** `Source = replacement` (:1168) or
  `PriorSource = prior` (:1177) runs. `replacement` is a plain local, not `await using`, so it is
  leaked. The same hole exists on the non-quiesce path (:1158 `SendHelloAsync` / :1159 `ReceiveAsync`).
- :1297 `ConnectedRuntime.DisposeAsync` then disposes `PriorSource`, `Source` (the prior), `Target`
  and finally `Context` (:1303) → `zlink_ctx_term` blocks on the leaked replacement.

The three hanging tests (`RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`,
`RouteAdmission_HandoverStartsFreshLivenessDeadline`,
`CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`) all go through
`HandoverAsync`; each hang is an admission timeout followed by this leak.

## 2. Spec clause

`core/doc/spec/core/socket/README.ko.md:486` — "Socket은 context가 종료되기 전에 `zlink_close()`로
닫아야 한다." `zlink_ctx_term()` blocking until every socket is closed is the specified behaviour
(`core/include/zlink/core/api.h:108–122`; `socket_base_t::process_stop`, socket_base_lifecycle.cpp:1310:
"The user is still responsible for calling zlink_close on the socket though!").

## 3. Public-C-API repro (worktree, uncommitted)

Worktree: `/home/hep7/project/zlink-core-term` (detached at `4c2521744a`), dev build in `core/build-dev`.

- `core/tests/integration/test_ctx_term_fixed_rid_handover.cpp` (new, public API only)
- `core/tests/CMakeLists.txt` (+7: registered after `test_ctx_destroy`; labels `integration;serial`; TIMEOUT 30)

Cases (ROUTER: HANDOVER RID policy, MANDATORY, linger 0, MAXMSGSIZE -1; DEALERs: 50-byte fixed RID;
per cycle: replacement DEALER connects with the same RID while the prior is alive, prior sends a stale
hello, prior closed with linger 0, current re-admitted; then source DEALER, ROUTER closed with linger 0;
`zlink_ctx_term` on a thread under a 10 s watchdog):

| case | result |
|---|---|
| `..._cycles_tcp` (prior reconnect disabled, as `HandoverAsync(quiescePrior:true)`) | ctx_term returns, no hang |
| `..._cycles_tcp_reconnect_alive` (prior reconnect intent left armed) | ctx_term returns, no hang |
| `..._cycles_inproc` | ctx_term returns, no hang |
| `test_ctx_term_blocks_until_unclosed_dealer_is_closed` (one DEALER intentionally left open) | ctx_term still blocked after 500 ms (as in the dump); after a late `zlink_close()` from another thread it returns 0. Note: `zlink_set_option` on that socket is refused with `ETERM` once termination began, `zlink_close` is accepted. |

Runs: 5/5 green standalone (4.0–6.6 s), 1/1 green via ctest. Iterating on ordering (linger -1 vs 0,
reconnect alive vs disabled, tcp vs inproc, 2–6 cycles) never produced a hang with all sockets closed.

## 4. Core fix

None required (no Core diff besides the regression/contract test above). Core conforms to the spec.

## 5. Gate counts (worktree `core/build-dev`, `ctest -j2`)

- `test_ctx_term_fixed_rid_handover`: 5/5 standalone, 1/1 ctest.
- `test_ctx_destroy`, `test_router_handover`: 2/2 passed (11.95 s).
- Full ctest / bindings gates not run: there is no Core source change (CONTRIBUTING §5 applies to Core commits).

## 6. Proposed framework-side fix (not applied; framework files are read-only for this job)

`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs`
- `HandoverAsync` (:1150–:1185) and `ReconnectAsync` (:1139–:1147): own `replacement` until it is
  published — wrap the admission (`SendHelloAsync`/`ReceiveAsync`/`SendHelloUntilAdmittedAsync`)
  in `try { ... } catch { await replacement.DisposeAsync(); throw; }`, and in `ReconnectAsync` do not
  overwrite `Source` before the old socket is disposed.
- Defensive: `ConnectedRuntime.DisposeAsync` (:1297) could track every DEALER it created and dispose
  all of them before `Context.DisposeAsync()`, so a failed test body never turns into a ctx_term hang.
- Optional binding hardening (out of scope): a finalizer on `Systems.Zlink.Runtime.Sockets.Internal.SocketHandle`
  would only mask the ordering bug and would not release ctx_term deterministically; not recommended.

## 7. Secondary finding / BLOCKERS

- **Why the admission times out (root trigger, separate Core question):** in the repro, a same-RID
  replacement DEALER over **tcp** with the prior pipe still alive is admitted only after
  100 ms – 2.9 s (measured probes: 200/600/100/400/100/2400/100/600/100/1100 ms; one run with the prior's
  reconnect armed did not admit within 5 s), while **inproc is admitted on the first 100 ms probe every
  time**. Setting the DEALERs' `RECONNECT_IVL`/`_MAX` to 10 ms makes it worse (no admission in 5 s), so it is
  handover churn between the live prior pipe and the replacement, not plain reconnect backoff. The
  framework helper's 2 s deadline (`SendHelloUntilAdmittedAsync`) sits inside that spread, which is why
  the leak fires intermittently. The repro test therefore bounds the pre-disconnect admission probe
  (600 ms, best effort) and asserts admission strictly only after the prior's exact disconnect. This
  latency deserves its own Core investigation (router handover of a same-RID DEALER on tcp).
- No other blockers. Main tree untouched except this document; `.NET` framework changes were not touched.
