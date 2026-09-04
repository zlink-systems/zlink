# Bucket F — .NET canonical fixture DEALER leak fix (2026-09-05)

## Change

`CanonicalActorJoinIngressReplyTests.ConnectedRuntime` now records the initial DEALER and every
replacement DEALER immediately after creation. `DisposeAsync` closes the complete recorded set
before disposing the managed mesh node and native context. A replacement that has not yet been
published through `Source` or `PriorSource` is therefore still closed when reconnect or handover
admission throws. Existing `Source` and `PriorSource` handover semantics are unchanged.

Only the test fixture was changed. No Core, binding, framework production source, contract, or
timeout was changed.

## Regression

`HandoverAdmissionFailure_DisposeClosesUnpublishedReplacement` injects the existing route-admission
`TimeoutException` after `HandoverAsync` creates its replacement and before it publishes that socket
as `Source`. The test then requires `ConnectedRuntime.DisposeAsync` to complete with
`WaitAsync(TimeSpan.FromSeconds(10))`.

Without tracking the unpublished replacement, this path leaves its native DEALER open and the
bounded teardown fails while `Context.DisposeAsync` remains blocked in `zlink_ctx_term`. With the
fix, the focused regression passed (1/1).

## Requested verification

Both runs used the requested environment, package-hash-specific NuGet cache, test filter,
`--blame-hang --blame-hang-timeout 3m`, and
`flock -w7200 /tmp/zlink-dotnet-gate.lock`.

- Run 1: passed, 15/15, 0 failed, 0 skipped, 25 s. No teardown hang. No admission failure.
- Run 2: passed, 15/15, 0 failed, 0 skipped, 25 s. No teardown hang. No admission failure.

An earlier focused attempt did not reach the regression body: fixture `CreateAsync` failed in 103 ms
with `System.TimeoutException : Route admission reply was not received.` at the initial
`ReceiveAsync`. A repeat of the focused regression passed. This is consistent with the separately
tracked TCP route-admission timing finding; it was not changed in this job.

## BLOCKERS

None for the DEALER ownership and teardown fix. The separate same-RID TCP replacement admission
latency remains outside this job's scope.
