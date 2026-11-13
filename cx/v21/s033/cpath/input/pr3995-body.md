# perf(dotnet): run RouteMesh channel handlers in the mailbox claim turn (#3869)

## Summary
.NET request path (#3869, #1639): remove the second per-channel application queue on the RouteMesh ChannelName path, so the receive-side handler runs in the existing mailbox claim turn.

**Why.** kairos measured the receive-side handoff in serial round trips on main with options A and C:
- every request changes thread between receive and handler
- queue wait, wake and claim take about 10.8µs (1KiB and 4KiB)
- the remaining dispatch work takes about 4.1µs

The path was: fixed receive worker → mailbox application worker (claims the ChannelName mailbox) → `ZLinkChannelApplicationDispatchQueue.PostAsync` → another application worker. The second post was a duplicate assignment. The ChannelName mailbox already owns order, claim and readiness. A read-only design study found that a .NET binding receive API (`SetReadableHandler`) would not remove this hop, because it needs its own watcher thread on Linux eventfd. So the fix is in the Framework, and the binding is unchanged (the user approved this direction).

**Change**
- `ZLinkMeshNodeRouteDispatcher`: channel requests dispatch directly from the mailbox claim turn. The per-channel `ZLinkChannelApplicationDispatchQueue` instances, their completion TCS, DTO and `DisposeAsync` are removed. The channel flow trace (Received/Admitted) moves into the dispatch path unchanged.
- Async results and drain lifetime stay with their existing owners. The pump retains incomplete dispatches (`ZLinkMeshDispatchPump` `Retain`), and the runtime task runner's `StopAsync` waits for them. The removed queue retained async dispatches the same way, so ordering semantics are unchanged: synchronous handlers on one channel run in order, and async completions are retained.
- Option A's immediate assignment is kept. The receiver still never runs handlers (hot-path §4.4), and no receive-role handoff is reintroduced.
- ClientServer and fanout, which share the queue type, are unchanged. There is no new public API, queue, flag or lock.

Receive → handler now takes **one** Framework thread change, previously two, the same as the binding.

## Tests
- New `MeshReceiveHandoffTests`:
  - the handler starts on the mailbox claim thread and turn, not the receive thread, and not via the channel queue
  - a blocked synchronous handler keeps the next same-channel request waiting while another channel proceeds
  - sealed admission returns `shutting_down`
  - drain waits for an in-flight async reply
  - with all application workers busy, the receiver and management stay independent
- Existing tests that disposed the removed dispatcher queue now stop through the runtime task runner. They are implementation-detail changes; their behavior, order and result assertions are unchanged.

## Verification (job base: main before #3987; merged cleanly with current main, which adds #3987)
- New tests 2 × 100 runs, plus 41 related tests.
- net8.0 Debug: Unit 4,532/4,532 · Contract 98 · Stream Connector 271.
- net10.0 Release: Unit 4,517/4,519. The 2 failures were artifact-path setup; they pass when rerun with in-repo artifacts.
- CSharpier, private-access and `git diff --check` pass. 3 timing-based perf tests were not run in the job, because measurement belongs to kairos. CI runs them.

kairos measures the 4 .NET cells after merge.

Reviewed line by line by the supervisor, including a comparison against the removed queue's `RunTurnAsync` retain semantics.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01D9jfevecSH9qJxjtX358f4

