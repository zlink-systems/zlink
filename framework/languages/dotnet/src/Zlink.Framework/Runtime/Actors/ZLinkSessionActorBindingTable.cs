namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkSessionBindingEntry(
    ZLinkSessionContext Context,
    string BindingToken,
    ZLinkSessionActor ActorRef,
    ulong BindingGeneration,
    ZLinkSessionBindingRoute Route,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    string? RelocationHandoffId = null,
    string? CompletedRelocationHandoffId = null,
    int ActiveFrames = 0,
    TaskCompletionSource? DrainSignal = null,
    TaskCompletionSource? RouteAvailableSignal = null)
{
    internal ulong ObjectGeneration => Route.Ref.ObjectGeneration;
    internal ulong AuthorityOwnerGeneration => Route.AuthorityOwnerGeneration;
    internal string MeshName => Route.MeshName;
    internal ulong TargetNodeGeneration => Route.TargetNodeGeneration;
    internal ulong OwnerLeaseGeneration => Route.OwnerLeaseGeneration;
}

internal readonly record struct ZLinkSessionBindingRoute
{
    private ZLinkSessionBindingRoute(
        ActorRef actor,
        string meshName,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        Ref = actor;
        MeshName = meshName;
        TargetNodeGeneration = targetNodeGeneration;
        AuthorityOwnerGeneration = authorityOwnerGeneration;
        OwnerLeaseGeneration = ownerLeaseGeneration;
    }

    internal ActorRef Ref { get; }
    internal string MeshName { get; }
    internal ulong TargetNodeGeneration { get; }
    internal ulong AuthorityOwnerGeneration { get; }
    internal ulong OwnerLeaseGeneration { get; }

    internal static ZLinkSessionBindingRoute Create(
        ActorRef actor,
        string meshName,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        if (!TryCreate(
                actor,
                meshName,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                out var route))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actor.ActorId}' binding requires an exact Mesh, node lifecycle, and owner lease.");
        return route;
    }

    internal static bool TryCreate(
        ActorRef actor,
        string meshName,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        out ZLinkSessionBindingRoute route)
    {
        if (string.IsNullOrWhiteSpace(actor.ActorId)
            || actor.ObjectGeneration == 0
            || actor.NodeRid.IsEmpty
            || string.IsNullOrWhiteSpace(meshName)
            || !string.Equals(actor.MeshName, meshName, StringComparison.Ordinal)
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0)
        {
            route = default;
            return false;
        }
        route = new ZLinkSessionBindingRoute(
            actor,
            meshName,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        return true;
    }

    internal bool MatchesFence(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        string meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration) =>
        string.Equals(Ref.ActorId, actorId, StringComparison.Ordinal)
        && Ref.ObjectGeneration == objectGeneration
        && AuthorityOwnerGeneration == authorityOwnerGeneration
        && string.Equals(MeshName, meshName, StringComparison.Ordinal)
        && TargetNodeGeneration == targetNodeGeneration
        && OwnerLeaseGeneration == ownerLeaseGeneration;
}

internal readonly record struct ZLinkSessionRouteSeal(
    string ActorId,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string MeshName,
    ulong TargetNodeGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    string HandoffId);

internal readonly record struct ZLinkSessionRouteSealResult(
    bool Acknowledged,
    ulong AcceptedHighWater);

internal readonly record struct ZLinkSessionRouteCommit(
    string ActorId,
    string BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong PreviousAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    string PreviousMeshName,
    string TargetMeshName,
    ulong PreviousTargetNodeGeneration,
    ulong TargetNodeGeneration,
    ulong PreviousOwnerLeaseGeneration,
    ulong TargetOwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    string HandoffId,
    ActorRef TargetActor);

internal readonly record struct ZLinkSessionRouteCommitResult(
    bool Acknowledged,
    ulong AcceptedHighWater);

internal readonly record struct ZLinkSessionBindingKey(
    string ActorId,
    string BindingToken);

internal readonly record struct ZLinkSessionBindingTombstone(
    RoutingId SessionRid,
    ulong BindingGeneration,
    ulong SessionOwnerNodeGeneration,
    ZLinkSessionBindingRoute ActorRoute,
    DateTimeOffset ExpiresAt);

internal sealed class ZLinkSessionActorBindingTable
{
    private readonly Dictionary<ZLinkSessionBindingKey, ZLinkSessionBindingEntry> _entries = new();
    private readonly Dictionary<ZLinkSessionBindingKey, ZLinkSessionBindingTombstone>
        _tombstones = new();
    private readonly TimeSpan _tombstoneRetention;
    private readonly TimeProvider _timeProvider;
    private readonly int _maxTombstones;

    public ZLinkSessionActorBindingTable(
        TimeSpan tombstoneRetention,
        TimeProvider? timeProvider = null,
        int maxTombstones = 4_096)
    {
        _tombstoneRetention = tombstoneRetention > TimeSpan.Zero
            ? tombstoneRetention
            : TimeSpan.FromSeconds(30);
        _timeProvider = timeProvider ?? TimeProvider.System;
        _maxTombstones = maxTombstones > 0
            ? maxTombstones
            : 4_096;
    }

    public ZLinkSessionBindingEntry[] Bind(
        string actorId,
        ZLinkSessionContext context,
        string bindingToken,
        ZLinkSessionActor actorRef,
        ulong bindingGeneration,
        ZLinkSessionBindingRoute route,
        ulong sessionOwnerNodeGeneration)
    {
        if (!string.Equals(actorId, route.Ref.ActorId, StringComparison.Ordinal))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorId}' binding route identifies a different Actor.");
        lock (_entries)
        {
            PurgeExpiredTombstones();
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (_tombstones.TryGetValue(key, out var tombstone))
            {
                var exact = tombstone.SessionRid == actorRef.SessionRid
                            && tombstone.BindingGeneration == bindingGeneration
                            && tombstone.SessionOwnerNodeGeneration
                            == sessionOwnerNodeGeneration
                            && tombstone.ActorRoute == route;
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    exact
                        ? $"Actor '{actorId}' session binding was replaced before local commit."
                        : $"Actor '{actorId}' reused a tombstoned binding token with conflicting identity fields.",
                    ZLinkRetryAdvice.DoNotRetry);
            }
            var replaced = _entries
                .Where(entry => string.Equals(entry.Key.ActorId, actorId, StringComparison.Ordinal))
                .Select(entry => entry.Value)
                .ToArray();
            foreach (var entry in replaced)
            {
                _entries.Remove(new ZLinkSessionBindingKey(actorId, entry.BindingToken));
                entry.DrainSignal?.TrySetResult();
                entry.RouteAvailableSignal?.TrySetResult();
            }

            _entries[key] = new ZLinkSessionBindingEntry(
                context,
                bindingToken,
                actorRef,
                bindingGeneration,
                route,
                sessionOwnerNodeGeneration,
                AcceptedHighWater: 0);
            return replaced;
        }
    }

    public void Tombstone(
        string actorId,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong sessionOwnerNodeGeneration,
        ZLinkSessionBindingRoute actorRoute)
    {
        lock (_entries)
        {
            PurgeExpiredTombstones();
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (_entries.TryGetValue(key, out var current)
                && (current.ActorRef.SessionRid != sessionRid
                    || current.BindingGeneration != bindingGeneration
                    || current.SessionOwnerNodeGeneration
                    != sessionOwnerNodeGeneration
                    || current.Route != actorRoute))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{actorId}' session binding tombstone does not match the current route.",
                    ZLinkRetryAdvice.DoNotRetry);
            if (_tombstones.TryGetValue(key, out var existingTombstone))
            {
                if (existingTombstone.SessionRid != sessionRid
                    || existingTombstone.BindingGeneration != bindingGeneration
                    || existingTombstone.SessionOwnerNodeGeneration
                    != sessionOwnerNodeGeneration
                    || existingTombstone.ActorRoute != actorRoute)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        $"Actor '{actorId}' session binding tombstone reused one token with conflicting identity fields.",
                        ZLinkRetryAdvice.DoNotRetry);
                return;
            }
            if (!_tombstones.ContainsKey(key)
                && _tombstones.Count >= _maxTombstones)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Session binding tombstone capacity is exhausted.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            _tombstones[key] = new ZLinkSessionBindingTombstone(
                sessionRid,
                bindingGeneration,
                sessionOwnerNodeGeneration,
                actorRoute,
                _timeProvider.GetUtcNow() + _tombstoneRetention);
            if (_entries.TryGetValue(key, out var entry))
            {
                _entries.Remove(key);
                entry.DrainSignal?.TrySetResult();
                entry.RouteAvailableSignal?.TrySetResult();
            }
        }
    }

    private void PurgeExpiredTombstones()
    {
        if (_tombstones.Count == 0) return;
        var now = _timeProvider.GetUtcNow();
        foreach (var key in _tombstones
                     .Where(entry => entry.Value.ExpiresAt <= now)
                     .Select(static entry => entry.Key)
                     .ToArray())
            _tombstones.Remove(key);
    }

    internal int TombstoneCount
    {
        get
        {
            lock (_entries)
            {
                PurgeExpiredTombstones();
                return _tombstones.Count;
            }
        }
    }

    public bool TryAccept(
        string actorId,
        string bindingToken,
        out ulong acceptedHighWater)
    {
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (!_entries.TryGetValue(key, out var entry))
            {
                //  두 거절 분기가 같은 문구로 나가면 어느 쪽인지 알 수 없다.
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"session_frame_refused reason=no_binding actor={actorId}");
                acceptedHighWater = 0;
                return false;
            }
            if (entry.RelocationHandoffId is not null)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"session_frame_refused reason=route_sealed actor={actorId} "
                    + $"handoff={entry.RelocationHandoffId} "
                    + $"accepted_high_water={entry.AcceptedHighWater}");
                acceptedHighWater = entry.AcceptedHighWater;
                return false;
            }
            acceptedHighWater = checked(entry.AcceptedHighWater + 1);
            _entries[key] = entry with
            {
                AcceptedHighWater = acceptedHighWater,
                ActiveFrames = checked(entry.ActiveFrames + 1)
            };
            return true;
        }
    }

    public ValueTask<bool> WaitForRouteAvailableAsync(
        string actorId,
        string bindingToken,
        CancellationToken cancellationToken)
    {
        Task? signalTask = null;
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (!_entries.TryGetValue(key, out var entry))
                return ValueTask.FromResult(false);
            if (entry.RelocationHandoffId is null)
                return ValueTask.FromResult(true);

            var signal = entry.RouteAvailableSignal
                         ?? new TaskCompletionSource(
                             TaskCreationOptions.RunContinuationsAsynchronously);
            _entries[key] = entry with { RouteAvailableSignal = signal };
            signalTask = signal.Task;
        }

        return WaitForRouteAvailableSignalAsync(signalTask, cancellationToken);
    }

    private static async ValueTask<bool> WaitForRouteAvailableSignalAsync(
        Task signalTask,
        CancellationToken cancellationToken)
    {
        await signalTask.WaitAsync(cancellationToken).ConfigureAwait(false);
        return true;
    }

    public void CompleteAccepted(
        string actorId,
        string bindingToken)
    {
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (!_entries.TryGetValue(key, out var entry)
                || entry.ActiveFrames == 0)
                return;
            var remaining = entry.ActiveFrames - 1;
            _entries[key] = entry with { ActiveFrames = remaining };
            if (remaining == 0)
                entry.DrainSignal?.TrySetResult();
        }
    }

    public async ValueTask<ZLinkSessionRouteSealResult> SealRouteAsync(
        ZLinkSessionRouteSeal request,
        CancellationToken cancellationToken)
    {
        //  Receiving side of the route seal. Paired with route_control_sent on
        //  the requester so a stalled seal shows which side never moved.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"route_seal_received actor={request.ActorId}");
        Task? drain = null;
        ulong acceptedHighWater;
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(
                request.ActorId,
                request.BindingToken);
            if (!_entries.TryGetValue(key, out var entry))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_seal_refused actor={request.ActorId} entry=false "
                    + "binding=false route=false session_owner=false");
                return new ZLinkSessionRouteSealResult(
                    false,
                    0);
            }
            var bindingMatches = entry.BindingGeneration == request.BindingGeneration;
            var routeMatches = entry.Route.MatchesFence(
                request.ActorId,
                request.ObjectGeneration,
                request.AuthorityOwnerGeneration,
                request.MeshName,
                request.TargetNodeGeneration,
                request.OwnerLeaseGeneration);
            var sessionOwnerMatches = entry.SessionOwnerNodeGeneration
                                      == request.SessionOwnerNodeGeneration;
            if (!bindingMatches || !routeMatches || !sessionOwnerMatches)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_seal_refused actor={request.ActorId} entry=true "
                    + $"binding={bindingMatches} route={routeMatches} "
                    + $"session_owner={sessionOwnerMatches} "
                    + $"current_authority={entry.AuthorityOwnerGeneration} "
                    + $"request_authority={request.AuthorityOwnerGeneration} "
                    + $"current_node={entry.TargetNodeGeneration} "
                    + $"request_node={request.TargetNodeGeneration} "
                    + $"current_lease={entry.OwnerLeaseGeneration} "
                    + $"request_lease={request.OwnerLeaseGeneration} "
                    + $"current_session_owner={entry.SessionOwnerNodeGeneration} "
                    + $"request_session_owner={request.SessionOwnerNodeGeneration}");
                return new ZLinkSessionRouteSealResult(
                    false,
                    entry.AcceptedHighWater);
            }

            if (entry.RelocationHandoffId is { } current
                && !string.Equals(current, request.HandoffId, StringComparison.Ordinal))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_seal_refused actor={request.ActorId} entry=true "
                    + "binding=true route=true session_owner=true "
                    + $"handoff=false current_handoff={current} "
                    + $"request_handoff={request.HandoffId}");
                return new ZLinkSessionRouteSealResult(
                    false,
                    entry.AcceptedHighWater);
            }

            var signal = entry.ActiveFrames == 0
                ? null
                : entry.DrainSignal
                  ?? new TaskCompletionSource(
                      TaskCreationOptions.RunContinuationsAsynchronously);
            _entries[key] = entry with
            {
                RelocationHandoffId = request.HandoffId,
                DrainSignal = signal
            };
            drain = signal?.Task;
            acceptedHighWater = entry.AcceptedHighWater;
            //  A non-zero ActiveFrames makes the seal wait for a drain signal
            //  that only frame completion raises. If a frame is never
            //  completed the seal never answers and the join times out.
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"route_seal_drain actor={request.ActorId} "
                + $"active_frames={entry.ActiveFrames} waits={drain is not null}");
        }
        if (drain is not null)
            await drain.WaitAsync(cancellationToken).ConfigureAwait(false);
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(
                request.ActorId,
                request.BindingToken);
            if (!_entries.TryGetValue(key, out var current))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_seal_refused actor={request.ActorId} entry=false "
                    + "binding=false route=false session_owner=false after_drain=true");
                return new ZLinkSessionRouteSealResult(false, acceptedHighWater);
            }
            var handoffMatches = string.Equals(
                current.RelocationHandoffId,
                request.HandoffId,
                StringComparison.Ordinal);
            if (!handoffMatches || current.ActiveFrames != 0)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_seal_refused actor={request.ActorId} entry=true "
                    + "binding=true route=true session_owner=true after_drain=true "
                    + $"handoff={handoffMatches} active_frames={current.ActiveFrames}");
                return new ZLinkSessionRouteSealResult(false, acceptedHighWater);
            }
            return new ZLinkSessionRouteSealResult(
                true,
                current.AcceptedHighWater);
        }
    }

    public bool AbortRouteSeal(ZLinkSessionRouteSeal request)
    {
        TaskCompletionSource? routeAvailableSignal = null;
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(
                request.ActorId,
                request.BindingToken);
            if (!_entries.TryGetValue(key, out var entry)
                || entry.BindingGeneration != request.BindingGeneration
                || !entry.Route.MatchesFence(
                    request.ActorId,
                    request.ObjectGeneration,
                    request.AuthorityOwnerGeneration,
                    request.MeshName,
                    request.TargetNodeGeneration,
                    request.OwnerLeaseGeneration)
                || entry.SessionOwnerNodeGeneration
                != request.SessionOwnerNodeGeneration
                || !string.Equals(
                    entry.RelocationHandoffId,
                    request.HandoffId,
                    StringComparison.Ordinal))
                return false;
            _entries[key] = entry with
            {
                RelocationHandoffId = null,
                DrainSignal = null,
                RouteAvailableSignal = null
            };
            routeAvailableSignal = entry.RouteAvailableSignal;
        }
        routeAvailableSignal?.TrySetResult();
        return true;
    }

    public bool UnsealCommittedRoute(
        ZLinkSessionRouteCommit request)
    {
        if (!ZLinkSessionBindingRoute.TryCreate(
                request.TargetActor,
                request.TargetMeshName,
                request.TargetNodeGeneration,
                request.TargetAuthorityOwnerGeneration,
                request.TargetOwnerLeaseGeneration,
                out var targetRoute))
            return false;
        TaskCompletionSource? routeAvailableSignal = null;
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(
                request.ActorId,
                request.BindingToken);
            if (!_entries.TryGetValue(key, out var entry))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_unseal_refused actor={request.ActorId} entry=false "
                    + "binding=false route=false session_owner=false high_water=false handoff=false");
                return false;
            }
            var bindingMatches = entry.BindingGeneration == request.BindingGeneration;
            var routeMatches = entry.Route == targetRoute;
            var sessionOwnerMatches = entry.SessionOwnerNodeGeneration
                                      == request.SessionOwnerNodeGeneration;
            var highWaterMatches = entry.AcceptedHighWater
                                   >= request.AcceptedHighWater;
            var handoffMatches = string.Equals(
                entry.RelocationHandoffId,
                request.HandoffId,
                StringComparison.Ordinal);
            var completedHandoffMatches = string.Equals(
                entry.CompletedRelocationHandoffId,
                request.HandoffId,
                StringComparison.Ordinal);
            if (!bindingMatches
                || !routeMatches
                || !sessionOwnerMatches
                || !highWaterMatches
                || (!handoffMatches && !completedHandoffMatches))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_unseal_refused actor={request.ActorId} "
                    + $"entry=true binding={bindingMatches} route={routeMatches} "
                    + $"session_owner={sessionOwnerMatches} high_water={highWaterMatches} "
                    + $"handoff={handoffMatches} completed={completedHandoffMatches}");
                return false;
            }
            //  The first unseal must clear the active handoff even though the
            //  commit has already recorded the same handoff as completed.
            //  Only a later duplicate, after the active handoff is gone, is
            //  the idempotent no-op.
            if (!handoffMatches && completedHandoffMatches)
                return true;
            _entries[key] = entry with
            {
                RelocationHandoffId = null,
                DrainSignal = null,
                RouteAvailableSignal = null
            };
            routeAvailableSignal = entry.RouteAvailableSignal;
        }
        routeAvailableSignal?.TrySetResult();
        return true;
    }

    public ZLinkSessionRouteCommitResult CommitRoute(
        ZLinkSessionRouteCommit request)
    {
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(
                request.ActorId,
                request.BindingToken);
            if (!_entries.TryGetValue(key, out var entry))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_refused actor={request.ActorId} entry=false "
                    + "binding=false object=false session_owner=false high_water=false "
                    + "route=false handoff=false completed=false");
                return new ZLinkSessionRouteCommitResult(
                    false,
                    0);
            }

            var bindingMatches = entry.BindingGeneration == request.BindingGeneration;
            var objectMatches = entry.ObjectGeneration == request.ObjectGeneration;
            var sessionOwnerMatches = entry.SessionOwnerNodeGeneration
                                      == request.SessionOwnerNodeGeneration;
            if (!bindingMatches
                || !objectMatches
                || !sessionOwnerMatches)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_refused actor={request.ActorId} entry=true "
                    + $"binding={bindingMatches} object={objectMatches} "
                    + $"session_owner={sessionOwnerMatches} high_water=false "
                    + $"current_high_water={entry.AcceptedHighWater} "
                    + $"request_high_water={request.AcceptedHighWater}");
                return new ZLinkSessionRouteCommitResult(
                    false,
                    entry.AcceptedHighWater);
            }

            var targetRouteCreated = ZLinkSessionBindingRoute.TryCreate(
                    request.TargetActor,
                    request.TargetMeshName,
                    request.TargetNodeGeneration,
                    request.TargetAuthorityOwnerGeneration,
                    request.TargetOwnerLeaseGeneration,
                    out var targetRoute);
            var targetObjectMatches = targetRouteCreated
                                       && targetRoute.Ref.ObjectGeneration
                                       == request.ObjectGeneration;
            var authorityOrderMatches = request.TargetAuthorityOwnerGeneration
                                        > request.PreviousAuthorityOwnerGeneration;
            if (!targetRouteCreated
                || !targetObjectMatches
                || !authorityOrderMatches)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_refused actor={request.ActorId} entry=true "
                    + "binding=true object=true session_owner=true high_water=true "
                    + $"target_route={targetRouteCreated} target_object={targetObjectMatches} "
                    + $"authority_order={authorityOrderMatches}");
                return new ZLinkSessionRouteCommitResult(
                    false,
                    entry.AcceptedHighWater);
            }

            var completedHandoffMatches = string.Equals(
                entry.CompletedRelocationHandoffId,
                request.HandoffId,
                StringComparison.Ordinal);
            // A completion retry can race with frames accepted after the first
            // commit. The target route and completed handoff are the durable
            // idempotency key; the returned high-water may therefore be newer
            // than the retried request. A different handoff still requires the
            // exact high-water fence below.
            if (entry.Route == targetRoute
                && completedHandoffMatches
                && entry.AcceptedHighWater >= request.AcceptedHighWater)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_idempotent actor={request.ActorId} "
                    + $"ack=true completed=true handoff={request.HandoffId} "
                    + $"current_high_water={entry.AcceptedHighWater} "
                    + $"request_high_water={request.AcceptedHighWater}");
                return new ZLinkSessionRouteCommitResult(
                    true,
                    entry.AcceptedHighWater);
            }

            var highWaterMatches = entry.AcceptedHighWater
                                   == request.AcceptedHighWater;
            if (!highWaterMatches)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_refused actor={request.ActorId} entry=true "
                    + $"binding=true object=true session_owner=true high_water=false "
                    + $"current_high_water={entry.AcceptedHighWater} "
                    + $"request_high_water={request.AcceptedHighWater}");
                return new ZLinkSessionRouteCommitResult(
                    false,
                    entry.AcceptedHighWater);
            }

            if (entry.Route == targetRoute)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_idempotent actor={request.ActorId} "
                    + $"ack={completedHandoffMatches} completed={completedHandoffMatches} "
                    + $"handoff={request.HandoffId}");
                return new ZLinkSessionRouteCommitResult(
                    completedHandoffMatches,
                    entry.AcceptedHighWater);
            }

            var previousRouteMatches = entry.Route.MatchesFence(
                    request.ActorId,
                    request.ObjectGeneration,
                    request.PreviousAuthorityOwnerGeneration,
                    request.PreviousMeshName,
                    request.PreviousTargetNodeGeneration,
                    request.PreviousOwnerLeaseGeneration);
            var handoffMatches = string.Equals(
                entry.RelocationHandoffId,
                request.HandoffId,
                StringComparison.Ordinal);
            if (!previousRouteMatches || !handoffMatches)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"route_commit_refused actor={request.ActorId} entry=true "
                    + "binding=true object=true session_owner=true high_water=true "
                    + $"previous_route={previousRouteMatches} handoff={handoffMatches} "
                    + $"completed={string.Equals(entry.CompletedRelocationHandoffId, request.HandoffId, StringComparison.Ordinal)} "
                    + $"current_authority={entry.AuthorityOwnerGeneration} "
                    + $"request_previous_authority={request.PreviousAuthorityOwnerGeneration} "
                    + $"current_node={entry.TargetNodeGeneration} "
                    + $"request_previous_node={request.PreviousTargetNodeGeneration}");
                return new ZLinkSessionRouteCommitResult(
                    false,
                    entry.AcceptedHighWater);
            }

            _entries[key] = entry with
            {
                Route = targetRoute,
                CompletedRelocationHandoffId = request.HandoffId
            };
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"route_commit_accepted actor={request.ActorId} handoff={request.HandoffId} "
                + $"high_water={entry.AcceptedHighWater}");
            return new ZLinkSessionRouteCommitResult(
                true,
                entry.AcceptedHighWater);
        }
    }

    public bool TryGet(
        string actorId,
        string bindingToken,
        out ZLinkSessionBindingEntry entry)
    {
        lock (_entries)
        {
            return _entries.TryGetValue(
                new ZLinkSessionBindingKey(actorId, bindingToken),
                out entry!);
        }
    }

    public bool TryGetRoute(
        string actorId,
        string bindingToken,
        ZLinkSessionActor actorRef,
        out ZLinkSessionBindingRoute route)
    {
        lock (_entries)
        {
            if (_entries.TryGetValue(
                    new ZLinkSessionBindingKey(actorId, bindingToken),
                    out var entry)
                && ReferenceEquals(entry.ActorRef, actorRef))
            {
                route = entry.Route;
                return true;
            }
            route = default;
            return false;
        }
    }

    public void Unbind(
        string actorId,
        ZLinkSessionContext context,
        string bindingToken)
    {
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (_entries.TryGetValue(key, out var existing)
                && ReferenceEquals(existing.Context, context)
                && string.Equals(existing.BindingToken, bindingToken, StringComparison.Ordinal))
            {
                _entries.Remove(key);
                existing.DrainSignal?.TrySetResult();
                existing.RouteAvailableSignal?.TrySetResult();
            }
        }
    }

    public bool TryGet(
        string actorId,
        string bindingToken,
        out ZLinkSessionContext context)
    {
        lock (_entries)
        {
            var key = new ZLinkSessionBindingKey(actorId, bindingToken);
            if (_entries.TryGetValue(key, out var entry))
            {
                context = entry.Context;
                return true;
            }

            context = null!;
            return false;
        }
    }

    public bool TryGetByActorId(
        string actorId,
        out ZLinkSessionContext context)
    {
        lock (_entries)
        {
            foreach (var entry in _entries)
                if (string.Equals(entry.Key.ActorId, actorId, StringComparison.Ordinal))
                {
                    context = entry.Value.Context;
                    return true;
                }

            context = null!;
            return false;
        }
    }

    public bool TryGetEntryByActorId(
        string actorId,
        out ZLinkSessionBindingEntry entry)
    {
        lock (_entries)
        {
            foreach (var candidate in _entries)
                if (string.Equals(
                        candidate.Key.ActorId,
                        actorId,
                        StringComparison.Ordinal))
                {
                    entry = candidate.Value;
                    return true;
                }
            entry = null!;
            return false;
        }
    }

    public IReadOnlyCollection<IZLinkSessionActor> SnapshotActors(
        ZLinkSessionContext context)
    {
        lock (_entries)
        {
            return _entries.Values
                .Where(entry => ReferenceEquals(entry.Context, context))
                .Select(static entry => (IZLinkSessionActor)entry.ActorRef)
                .ToArray();
        }
    }

    public ZLinkSessionActor? FindActor(
        ZLinkSessionContext context,
        string actorId)
    {
        lock (_entries)
        {
            return _entries.Values
                .Where(entry => ReferenceEquals(entry.Context, context))
                .FirstOrDefault(entry => string.Equals(
                    entry.ActorRef.ActorId,
                    actorId,
                    StringComparison.Ordinal))
                ?.ActorRef;
        }
    }

    public void ResetGeneration()
    {
        lock (_entries)
        {
            foreach (var entry in _entries.Values)
            {
                entry.DrainSignal?.TrySetResult();
                entry.RouteAvailableSignal?.TrySetResult();
            }
            _entries.Clear();
            _tombstones.Clear();
        }
    }
}
