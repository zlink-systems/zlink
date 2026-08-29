using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkSessionBindingEntry(
    ZLinkSessionContext Context,
    string BindingToken,
    ZLinkSessionActor ActorRef,
    ulong BindingGeneration,
    ZLinkSessionBindingRoute Route,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    RoutingId SessionOwnerNodeRid = default,
    string SessionOwnerId = "",
    ulong SessionOwnerLeaseGeneration = 0,
    string? RelocationHandoffId = null,
    string? CompletedRelocationHandoffId = null,
    int ActiveFrames = 0,
    TaskCompletionSource? DrainSignal = null,
    TaskCompletionSource? RouteAvailableSignal = null,
    ZLinkServiceWireCodec.SessionRelocationSealRecord?
        CanonicalRelocationSeal = null,
    ZLinkServiceWireCodec.SessionRelocationSealedRecord?
        CanonicalRelocationSealResult = null,
    ZLinkServiceWireCodec.SessionRelocationRouteRecord?
        AppliedCanonicalRelocationRoute = null)
{
    internal ulong ObjectGeneration => Route.Ref.ObjectGeneration;
    internal ulong AuthorityOwnerGeneration => Route.AuthorityOwnerGeneration;
    internal string MeshName => Route.MeshName.Value;
    internal ulong TargetNodeGeneration => Route.TargetNodeGeneration;
    internal ulong OwnerLeaseGeneration => Route.OwnerLeaseGeneration;
}

internal readonly record struct ZLinkSessionBindingRoute
{
    private ZLinkSessionBindingRoute(
        ActorRef actor,
        ZLinkMeshName meshName,
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
    internal ZLinkMeshName MeshName { get; }
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
        out ZLinkSessionBindingRoute route) =>
        TryCreateCore(
            actor,
            meshName,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            requireOwnerLease: true,
            out route);

    internal static bool TryCreateRelocated(
        ActorRef actor,
        string meshName,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        out ZLinkSessionBindingRoute route) =>
        TryCreateCore(
            actor,
            meshName,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            requireOwnerLease: false,
            out route);

    private static bool TryCreateCore(
        ActorRef actor,
        string meshName,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        bool requireOwnerLease,
        out ZLinkSessionBindingRoute route)
    {
        if (string.IsNullOrWhiteSpace(actor.ActorId)
            || actor.ObjectGeneration == 0
            || actor.NodeRid.IsEmpty
            || string.IsNullOrWhiteSpace(meshName)
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || requireOwnerLease && ownerLeaseGeneration == 0)
        {
            route = default;
            return false;
        }
        var runtimeMeshName = ZLinkMeshName.FromBoundary(
            meshName,
            nameof(meshName));
        if (!string.Equals(
                actor.MeshName,
                runtimeMeshName.Value,
                StringComparison.Ordinal))
        {
            route = default;
            return false;
        }
        route = new ZLinkSessionBindingRoute(
            actor,
            runtimeMeshName,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        return true;
    }

    internal bool MatchesFence(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkMeshName meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration) =>
        string.Equals(Ref.ActorId, actorId, StringComparison.Ordinal)
        && Ref.ObjectGeneration == objectGeneration
        && AuthorityOwnerGeneration == authorityOwnerGeneration
        && MeshName == meshName
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

internal readonly record struct ZLinkSessionFrameAcceptance(
    bool Accepted,
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
    ZLinkActorId ActorId,
    string BindingToken)
{
    internal static ZLinkSessionBindingKey FromBoundary(
        string actorId,
        string bindingToken) =>
        new(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            bindingToken);
}

internal readonly record struct ZLinkSessionOutboundTenure(
    string ActorId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    string BindingToken,
    ulong BindingGeneration,
    ulong SessionOwnerNodeGeneration,
    RoutingId SessionRid);

internal readonly record struct ZLinkSessionOutboundTenureProof(
    ZLinkSessionOutboundTenure Tenure,
    string OwnerId);

internal enum ZLinkSessionOutboundAdmissionKind
{
    Immediate,
    Retained,
    ProofRequired,
    NoBinding,
    WrongSession,
    Backpressured
}

internal enum ZLinkSessionOutboundDelivery
{
    Delivered,
    Backpressured,
    Discarded
}

internal readonly record struct ZLinkSessionOutboundAdmission(
    ZLinkSessionOutboundAdmissionKind Kind,
    ZLinkSessionOutboundCapability? Capability = null);

internal sealed class ZLinkSessionOutboundCapability(
    ZLinkSessionContext context,
    byte[] frame)
{
    private readonly TaskCompletionSource<ZLinkSessionOutboundDelivery>
        _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private int _settled;

    internal Task<ZLinkSessionOutboundDelivery> Completion => _completion.Task;

    internal ZLinkSessionOutboundDelivery Settle(bool deliver)
    {
        if (Interlocked.Exchange(ref _settled, 1) != 0)
            return _completion.Task.IsCompletedSuccessfully
                ? _completion.Task.Result
                : ZLinkSessionOutboundDelivery.Discarded;

        var result = ZLinkSessionOutboundDelivery.Discarded;
        if (deliver)
        {
            try
            {
                using var message = Message.From(frame);
                result = context.Write(message)
                    ? ZLinkSessionOutboundDelivery.Delivered
                    : ZLinkSessionOutboundDelivery.Backpressured;
            }
            catch
            {
                result = ZLinkSessionOutboundDelivery.Backpressured;
            }
        }
        _completion.TrySetResult(result);
        return result;
    }
}

internal readonly record struct ZLinkSessionBindingTombstone(
    RoutingId SessionRid,
    ulong BindingGeneration,
    ulong SessionOwnerNodeGeneration,
    ZLinkSessionBindingRoute ActorRoute,
    DateTimeOffset ExpiresAt);

internal sealed class ZLinkSessionActorBindingTable
{
    private const int DefaultMaxRetainedOutbound = 4_096;
    private static readonly Action<ILogger, string, string, Exception?>
        LateSessionRouteUpdate = LoggerMessage.Define<string, string>(
            LogLevel.Warning,
            new EventId(28002, "late_session_route_update"),
            "late_session_route_update actor={ActorId} relocation={RelocationId}");
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<ZLinkSessionBindingKey, ZLinkSessionBindingEntry> _entries = new();
    private readonly Dictionary<ZLinkSessionBindingKey, ZLinkSessionBindingTombstone>
        _tombstones = new();
    private readonly Dictionary<ZLinkSessionBindingKey, SessionBindingOutboundState>
        _outbound = new();
    private readonly Dictionary<ZLinkSessionBindingKey, CanonicalSealTimeoutState>
        _canonicalSealTimeouts = new();
    private readonly HashSet<CanonicalRelocationKey>
        _timedOutCanonicalSeals = [];
    private readonly Queue<CanonicalRelocationKey>
        _timedOutCanonicalSealOrder = new();
    private readonly TimeSpan _tombstoneRetention;
    private readonly TimeSpan _canonicalRelocationSealTimeout;
    private readonly TimeProvider _timeProvider;
    private readonly int _maxTombstones;
    private readonly int _maxRetainedOutbound;
    private readonly ILogger? _logger;

    private sealed class CanonicalSealTimeoutState(
        ZLinkServiceWireCodec.SessionRelocationSealRecord seal)
    {
        internal ZLinkServiceWireCodec.SessionRelocationSealRecord Seal { get; }
            = seal;
        internal CancellationTokenSource Cancellation { get; } = new();
        internal Task? Operation { get; set; }
    }

    private readonly record struct CanonicalRelocationKey(
        ZLinkServiceWireCodec.RelocationWireId RelocationId,
        ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator,
        ZLinkServiceWireCodec.SessionActorIdentityRecord Actor,
        ZLinkServiceWireCodec.SessionOwnerFenceRecord Session)
    {
        internal static CanonicalRelocationKey From(
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal) =>
            new(
                seal.RelocationId,
                seal.Coordinator,
                seal.Actor.Actor,
                seal.Session);

        internal static CanonicalRelocationKey From(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord route) =>
            new(
                route.RelocationId,
                route.Coordinator,
                route.Actor,
                route.Session);
    }

    public ZLinkSessionActorBindingTable(
        TimeSpan tombstoneRetention,
        TimeSpan canonicalRelocationSealTimeout,
        TimeProvider? timeProvider = null,
        int maxTombstones = 4_096,
        int maxRetainedOutbound = DefaultMaxRetainedOutbound,
        ILogger? logger = null)
    {
        _tombstoneRetention = tombstoneRetention > TimeSpan.Zero
            ? tombstoneRetention
            : TimeSpan.FromSeconds(30);
        _timeProvider = timeProvider ?? TimeProvider.System;
        _maxTombstones = maxTombstones > 0
            ? maxTombstones
            : 4_096;
        _maxRetainedOutbound = maxRetainedOutbound > 0
            ? maxRetainedOutbound
            : DefaultMaxRetainedOutbound;
        _logger = logger;
        _canonicalRelocationSealTimeout = canonicalRelocationSealTimeout;
        if (_canonicalRelocationSealTimeout <= TimeSpan.Zero
            || _canonicalRelocationSealTimeout == Timeout.InfiniteTimeSpan
            || _canonicalRelocationSealTimeout.Ticks
               % TimeSpan.TicksPerMillisecond != 0)
            throw new ArgumentOutOfRangeException(
                nameof(canonicalRelocationSealTimeout),
                "Session relocation seal timeout must be a positive duration "
                + "representable as exact whole milliseconds.");
    }

    private void ArmCanonicalSealTimeout(
        ZLinkSessionBindingKey key,
        ZLinkServiceWireCodec.SessionRelocationSealRecord seal)
    {
        if (_canonicalSealTimeouts.ContainsKey(key)) return;
        var timeout = new CanonicalSealTimeoutState(seal);
        _canonicalSealTimeouts.Add(key, timeout);
        using (ExecutionContext.SuppressFlow())
            timeout.Operation = RunCanonicalSealTimeoutAsync(key, timeout);
    }

    private void CancelCanonicalSealTimeout(ZLinkSessionBindingKey key)
    {
        if (!_canonicalSealTimeouts.Remove(key, out var timeout)) return;
        timeout.Cancellation.Cancel();
    }

    private void AddTimedOutCanonicalSeal(
        ZLinkServiceWireCodec.SessionRelocationSealRecord seal)
    {
        var key = CanonicalRelocationKey.From(seal);
        if (!_timedOutCanonicalSeals.Add(key)) return;
        _timedOutCanonicalSealOrder.Enqueue(key);
        while (_timedOutCanonicalSeals.Count > _maxTombstones)
            _timedOutCanonicalSeals.Remove(
                _timedOutCanonicalSealOrder.Dequeue());
    }

    private void LogLateSessionRouteUpdate(
        ZLinkServiceWireCodec.SessionRelocationRouteRecord request)
    {
        var logger = _logger;
        if (logger is null) return;
        try
        {
            if (logger.IsEnabled(LogLevel.Warning))
                LateSessionRouteUpdate(
                    logger,
                    request.Actor.ActorId,
                    request.RelocationId.ToString(),
                    null);
        }
        catch
        {
            // Diagnostics must not change command 44's one-way semantics.
        }
    }

    private async Task RunCanonicalSealTimeoutAsync(
        ZLinkSessionBindingKey key,
        CanonicalSealTimeoutState timeout)
    {
        try
        {
            await Task.Delay(
                    _canonicalRelocationSealTimeout,
                    _timeProvider,
                    timeout.Cancellation.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (timeout.Cancellation.IsCancellationRequested)
        {
            timeout.Cancellation.Dispose();
            return;
        }

        List<ZLinkSessionBindingEntry> timedOut = [];
        List<ZLinkSessionOutboundCapability> retained = [];
        var ownsTimeout = false;
        await _lane.RunAsync(() =>
        {
            ownsTimeout = _canonicalSealTimeouts.TryGetValue(
                    key,
                    out var current)
                && ReferenceEquals(current, timeout);
            if (ownsTimeout)
            {
                _canonicalSealTimeouts.Remove(key);
                if (_entries.TryGetValue(key, out var entry)
                    && entry.CanonicalRelocationSeal == timeout.Seal)
                {
                    AddTimedOutCanonicalSeal(timeout.Seal);
                    var sessionKeys = _entries
                        .Where(candidate =>
                            ReferenceEquals(
                                candidate.Value.Context,
                                entry.Context))
                        .Select(static candidate => candidate.Key)
                        .ToArray();
                    foreach (var sessionKey in sessionKeys)
                    {
                        var sessionEntry = _entries[sessionKey];
                        if (sessionEntry.CanonicalRelocationSeal is { } seal)
                            AddTimedOutCanonicalSeal(seal);
                        CancelCanonicalSealTimeout(sessionKey);
                        _entries.Remove(sessionKey);
                        retained.AddRange(RemoveOutbound(sessionKey));
                        timedOut.Add(sessionEntry);
                    }
                }
            }
        }).ConfigureAwait(false);

        timeout.Cancellation.Dispose();
        if (!ownsTimeout || timedOut.Count == 0) return;
        foreach (var entry in timedOut)
        {
            entry.DrainSignal?.TrySetResult();
            entry.RouteAvailableSignal?.TrySetResult();
        }
        SettleOutbound(retained, deliver: false);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"session_relocation_seal_timeout "
            + $"actor={key.ActorId.Value} "
            + $"relocation={timeout.Seal.RelocationId}");
        try
        {
            await timedOut[0].Context.CloseAsync().ConfigureAwait(false);
        }
        catch (Exception failure)
        {
            timedOut[0].Context.Runtime.ErrorSink.ReportRuntimeTaskException(
                $"session-relocation-seal-timeout:"
                + timedOut[0].Context.SessionId,
                failure);
        }
    }

    internal ValueTask<ZLinkSessionOutboundAdmission> AdmitOutboundAsync(
        ZLinkSessionOutboundTenure tenure,
        ZLinkSessionOutboundTenureProof? firstProof,
        byte[] frame)
    {
        ArgumentNullException.ThrowIfNull(frame);
        return _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(
                tenure.ActorId,
                tenure.BindingToken);
            if (!_entries.TryGetValue(key, out var entry))
                return new ZLinkSessionOutboundAdmission(
                    _entries.Keys.Any(candidate =>
                        candidate.ActorId.Value == tenure.ActorId)
                        ? ZLinkSessionOutboundAdmissionKind.WrongSession
                        : ZLinkSessionOutboundAdmissionKind.NoBinding);
            if (!MatchesPhysicalSession(entry, tenure))
                return new ZLinkSessionOutboundAdmission(
                    ZLinkSessionOutboundAdmissionKind.WrongSession);

            var capability = new ZLinkSessionOutboundCapability(
                entry.Context,
                frame);
            if (MatchesOutboundTenure(entry.Route, tenure)
                || MatchesLegacyProvisionalTenure(entry, tenure))
                return new ZLinkSessionOutboundAdmission(
                    ZLinkSessionOutboundAdmissionKind.Immediate,
                    capability);

            var unresolvedRelocatedTenure =
                entry.OwnerLeaseGeneration == 0
                && entry.ObjectGeneration == tenure.ObjectGeneration
                && entry.AuthorityOwnerGeneration
                == tenure.AuthorityOwnerGeneration
                && entry.TargetNodeGeneration == tenure.TargetNodeGeneration
                && entry.Route.Ref.NodeRid == tenure.TargetNodeRid
                && string.Equals(
                    entry.MeshName,
                    tenure.MeshName,
                    StringComparison.Ordinal);
            if (unresolvedRelocatedTenure)
            {
                if (firstProof is not { } relocationProof
                    || relocationProof.Tenure != tenure
                    || string.IsNullOrWhiteSpace(relocationProof.OwnerId)
                    || !ZLinkSessionBindingRoute.TryCreate(
                        entry.Route.Ref,
                        entry.MeshName,
                        entry.TargetNodeGeneration,
                        entry.AuthorityOwnerGeneration,
                        tenure.OwnerLeaseGeneration,
                        out var provenRoute))
                    return new ZLinkSessionOutboundAdmission(
                        ZLinkSessionOutboundAdmissionKind.ProofRequired);
                _entries[key] = entry with { Route = provenRoute };
                return new ZLinkSessionOutboundAdmission(
                    ZLinkSessionOutboundAdmissionKind.Immediate,
                    capability);
            }

            if (tenure.AuthorityOwnerGeneration
                    <= entry.AuthorityOwnerGeneration
                || tenure.TargetNodeRid.IsEmpty
                || tenure.TargetNodeGeneration == 0
                || tenure.OwnerLeaseGeneration == 0
                || string.IsNullOrWhiteSpace(tenure.MeshName))
                return new ZLinkSessionOutboundAdmission(
                    ZLinkSessionOutboundAdmissionKind.WrongSession);

            if (!_outbound.TryGetValue(key, out var outbound))
            {
                if (firstProof is not { } candidate
                    || candidate.Tenure != tenure
                    || string.IsNullOrWhiteSpace(candidate.OwnerId))
                    return new ZLinkSessionOutboundAdmission(
                        ZLinkSessionOutboundAdmissionKind.ProofRequired);
                outbound = new SessionBindingOutboundState();
                outbound.PendingTenureProof = candidate;
                _outbound.Add(key, outbound);
            }
            else if (outbound.PendingTenureProof is not { } acceptedProof)
            {
                if (firstProof is not { } candidate
                    || candidate.Tenure != tenure
                    || string.IsNullOrWhiteSpace(candidate.OwnerId))
                    return new ZLinkSessionOutboundAdmission(
                        ZLinkSessionOutboundAdmissionKind.ProofRequired);
                outbound.PendingTenureProof = candidate;
            }
            else if (acceptedProof.Tenure != tenure)
            {
                return new ZLinkSessionOutboundAdmission(
                    ZLinkSessionOutboundAdmissionKind.WrongSession);
            }

            if (outbound.Retained.Count >= _maxRetainedOutbound)
                return new ZLinkSessionOutboundAdmission(
                    ZLinkSessionOutboundAdmissionKind.Backpressured);
            outbound.Retained.Enqueue(capability);
            return new ZLinkSessionOutboundAdmission(
                ZLinkSessionOutboundAdmissionKind.Retained,
                capability);
        });
    }

    internal ValueTask<ZLinkSessionOutboundTenureProof?>
        GetMemoizedOutboundProofAsync(
            ZLinkSessionOutboundTenure tenure) =>
        _lane.RunAsync<ZLinkSessionOutboundTenureProof?>(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(
                tenure.ActorId,
                tenure.BindingToken);
            if (_entries.TryGetValue(key, out var entry)
                && MatchesPhysicalSession(entry, tenure)
                && _outbound.TryGetValue(key, out var outbound)
                && outbound.PendingTenureProof is { } accepted
                && accepted.Tenure == tenure)
            {
                return accepted;
            }
            return null;
        });

    private static bool MatchesCanonicalOutboundProof(
        ZLinkSessionBindingEntry entry,
        ZLinkSessionBindingKey key,
        ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
        ZLinkSessionRelocationAuthenticatedRoute authenticatedCandidate,
        ZLinkSessionOutboundTenureProof proof)
    {
        var tenure = proof.Tenure;
        return MatchesPhysicalSession(entry, tenure)
               && string.Equals(
                   tenure.BindingToken,
                   key.BindingToken,
                   StringComparison.Ordinal)
               && tenure.ActorId == route.Actor.ActorId
               && tenure.ObjectGeneration == route.Actor.ObjectGeneration
               && tenure.BindingGeneration == route.Session.BindingGeneration
               && tenure.SessionOwnerNodeGeneration
               == route.Session.SessionOwnerNodeGeneration
               && tenure.SessionRid == route.Session.SessionRid
               && tenure.TargetNodeRid == route.Route.TargetNodeRid
               && tenure.TargetNodeRid == authenticatedCandidate.NodeRid
               && tenure.TargetNodeGeneration
               == route.Route.TargetNodeGeneration
               && tenure.TargetNodeGeneration
               == authenticatedCandidate.NodeGeneration
               && tenure.AuthorityOwnerGeneration
               == route.Route.TargetAuthorityOwnerGeneration
               && tenure.AuthorityOwnerGeneration
               == authenticatedCandidate.AuthorityOwnerGeneration
               && tenure.OwnerLeaseGeneration > 0
               && string.Equals(
                   tenure.MeshName,
                   authenticatedCandidate.MeshName,
                   StringComparison.Ordinal)
               && !string.IsNullOrWhiteSpace(proof.OwnerId);
    }

    private static bool MatchesPhysicalSession(
        ZLinkSessionBindingEntry entry,
        ZLinkSessionOutboundTenure tenure) =>
        entry.ObjectGeneration == tenure.ObjectGeneration
        && entry.BindingGeneration == tenure.BindingGeneration
        && entry.SessionOwnerNodeGeneration
        == tenure.SessionOwnerNodeGeneration
        && entry.Context.RoutingId is { } sessionRid
        && sessionRid == tenure.SessionRid;

    private static bool MatchesOutboundTenure(
        ZLinkSessionBindingRoute route,
        ZLinkSessionOutboundTenure tenure) =>
        route.MatchesFence(
            tenure.ActorId,
            tenure.ObjectGeneration,
            tenure.AuthorityOwnerGeneration,
            ZLinkMeshName.FromBoundary(
                tenure.MeshName,
                nameof(tenure.MeshName)),
            tenure.TargetNodeGeneration,
            tenure.OwnerLeaseGeneration)
        && route.Ref.NodeRid == tenure.TargetNodeRid;

    private static bool MatchesLegacyProvisionalTenure(
        ZLinkSessionBindingEntry entry,
        ZLinkSessionOutboundTenure tenure) =>
        entry.RelocationHandoffId is not null
        && entry.ObjectGeneration == tenure.ObjectGeneration
        && (entry.AuthorityOwnerGeneration
                != tenure.AuthorityOwnerGeneration
            || entry.TargetNodeGeneration != tenure.TargetNodeGeneration
            || entry.OwnerLeaseGeneration != tenure.OwnerLeaseGeneration);

    private List<ZLinkSessionOutboundCapability> RemoveOutbound(
        ZLinkSessionBindingKey key)
    {
        if (!_outbound.Remove(key, out var outbound))
            return [];
        var retained = new List<ZLinkSessionOutboundCapability>(
            outbound.Retained.Count);
        while (outbound.Retained.TryDequeue(out var capability))
            retained.Add(capability);
        return retained;
    }

    private static void SettleOutbound(
        IEnumerable<ZLinkSessionOutboundCapability> retained,
        bool deliver)
    {
        foreach (var capability in retained)
            capability.Settle(deliver);
    }

    private sealed class SessionBindingOutboundState
    {
        internal ZLinkSessionOutboundTenureProof? PendingTenureProof
            { get; set; }
        internal Queue<ZLinkSessionOutboundCapability> Retained { get; } = new();
    }

    public ValueTask<ZLinkSessionBindingEntry[]> BindAsync(
        ZLinkActorId actorId,
        ZLinkSessionContext context,
        string bindingToken,
        ZLinkSessionActor actorRef,
        ulong bindingGeneration,
        ZLinkSessionBindingRoute route,
        ulong sessionOwnerNodeGeneration,
        RoutingId sessionOwnerNodeRid = default,
        string sessionOwnerId = "",
        ulong sessionOwnerLeaseGeneration = 0)
    {
        if (!string.Equals(actorId.Value, route.Ref.ActorId, StringComparison.Ordinal))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorId}' binding route identifies a different Actor.");
        return _lane.RunAsync(() =>
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
                .Where(entry => entry.Key.ActorId == key.ActorId)
                .Select(entry => entry.Value)
                .ToArray();
            foreach (var entry in replaced)
            {
                var replacedKey = new ZLinkSessionBindingKey(
                    actorId,
                    entry.BindingToken);
                CancelCanonicalSealTimeout(replacedKey);
                _entries.Remove(replacedKey);
                SettleOutbound(RemoveOutbound(replacedKey), deliver: false);
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
                AcceptedHighWater: 0,
                SessionOwnerNodeRid: sessionOwnerNodeRid,
                SessionOwnerId: string.IsNullOrWhiteSpace(sessionOwnerId)
                    ? (sessionOwnerNodeRid.IsEmpty
                        ? actorRef.SessionRid.ToHex()
                        : sessionOwnerNodeRid.ToHex())
                    : sessionOwnerId,
                SessionOwnerLeaseGeneration:
                    sessionOwnerLeaseGeneration == 0
                        ? sessionOwnerNodeGeneration
                        : sessionOwnerLeaseGeneration);
            return replaced;
        });
    }

    public ValueTask TombstoneAsync(
        string actorId,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong sessionOwnerNodeGeneration,
        ZLinkSessionBindingRoute actorRoute)
        => _lane.RunAsync(() =>
        {
            PurgeExpiredTombstones();
            var key = ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
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
                CancelCanonicalSealTimeout(key);
                _entries.Remove(key);
                SettleOutbound(RemoveOutbound(key), deliver: false);
                entry.DrainSignal?.TrySetResult();
                entry.RouteAvailableSignal?.TrySetResult();
            }
        });

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

    internal ValueTask<int> GetTombstoneCountAsync() =>
        _lane.RunAsync(() =>
        {
            PurgeExpiredTombstones();
            return _tombstones.Count;
        });

    public ValueTask<ZLinkSessionFrameAcceptance?> AcceptAsync(
        string actorId,
        string bindingToken) =>
        _lane.RunAsync<ZLinkSessionFrameAcceptance?>(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
            if (!_entries.TryGetValue(key, out var entry))
            {
                //  두 거절 분기가 같은 문구로 나가면 어느 쪽인지 알 수 없다.
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"session_frame_refused reason=no_binding actor={actorId}");
                return new ZLinkSessionFrameAcceptance(false, 0);
            }
            if (entry.RelocationHandoffId is not null
                || entry.CanonicalRelocationSeal is not null)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"session_frame_refused reason=route_sealed actor={actorId} "
                    + $"handoff={entry.RelocationHandoffId} "
                    + $"accepted_high_water={entry.AcceptedHighWater}");
                return new ZLinkSessionFrameAcceptance(
                    false,
                    entry.AcceptedHighWater);
            }
            var acceptedHighWater = checked(entry.AcceptedHighWater + 1);
            _entries[key] = entry with
            {
                AcceptedHighWater = acceptedHighWater,
                ActiveFrames = checked(entry.ActiveFrames + 1)
            };
            return new ZLinkSessionFrameAcceptance(
                true,
                acceptedHighWater);
        });

    public async ValueTask<bool> WaitForRouteAvailableAsync(
        string actorId,
        string bindingToken,
        CancellationToken cancellationToken)
    {
        var wait = await _lane.RunAsync(() =>
        {
            Task? signalTask = null;
            var key = ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
            if (!_entries.TryGetValue(key, out var entry))
                return ValueTask.FromResult(false);
            if (entry.RelocationHandoffId is null
                && entry.CanonicalRelocationSeal is null)
                return ValueTask.FromResult(true);

            var signal = entry.RouteAvailableSignal
                         ?? new TaskCompletionSource(
                             TaskCreationOptions.RunContinuationsAsynchronously);
            _entries[key] = entry with { RouteAvailableSignal = signal };
            signalTask = signal.Task;
            return WaitForRouteAvailableSignalAsync(signalTask, cancellationToken);
        }).ConfigureAwait(false);
        return await wait.ConfigureAwait(false);
    }

    private static async ValueTask<bool> WaitForRouteAvailableSignalAsync(
        Task signalTask,
        CancellationToken cancellationToken)
    {
        await signalTask.WaitAsync(cancellationToken).ConfigureAwait(false);
        return true;
    }

    public ValueTask CompleteAcceptedAsync(
        string actorId,
        string bindingToken) =>
        _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
            if (!_entries.TryGetValue(key, out var entry)
                || entry.ActiveFrames == 0)
                return;
            var remaining = entry.ActiveFrames - 1;
            _entries[key] = entry with { ActiveFrames = remaining };
            if (remaining == 0)
                entry.DrainSignal?.TrySetResult();
        });

    internal async ValueTask<
        ZLinkServiceWireCodec.SessionRelocationSealedRecord>
        SealCanonicalRouteAsync(
            ZLinkServiceWireCodec.SessionRelocationSealRecord request,
            CancellationToken cancellationToken)
    {
        Task? drain = null;
        var installedResult = await _lane.RunAsync(() =>
                PrepareCanonicalRouteSeal(request, out drain))
            .ConfigureAwait(false);
        if (installedResult is { } completed)
            return completed;

        if (drain is not null)
            await drain.WaitAsync(cancellationToken).ConfigureAwait(false);

        return await _lane.RunAsync(() =>
        {
            if (!TryFindCanonicalBinding(
                    request.Actor.Actor,
                    request.Session,
                    out var key,
                    out var current)
                || current.CanonicalRelocationSeal != request
                || current.ActiveFrames != 0)
                throw new InvalidDataException(
                    "The command 42 binding changed while accepted frames drained.");
            if (current.CanonicalRelocationSealResult is { } installedResult)
                return installedResult;
            var result = new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
                request.RelocationId,
                request.Coordinator,
                request.Actor,
                request.Session);
            _entries[key] = current with
            {
                CanonicalRelocationSealResult = result
            };
            return result;
        }).ConfigureAwait(false);
    }

    private ZLinkServiceWireCodec.SessionRelocationSealedRecord?
        PrepareCanonicalRouteSeal(
            ZLinkServiceWireCodec.SessionRelocationSealRecord request,
            out Task? drain)
    {
        if (!TryFindCanonicalBinding(
                request.Actor.Actor,
                request.Session,
                out var key,
                out var entry)
            || !MatchesCanonicalActorRoute(entry, request.Actor))
            throw new InvalidDataException(
                "Command 42 does not match the current session binding.");
        if (entry.RelocationHandoffId is not null)
            throw new InvalidDataException(
                "Command 42 conflicts with a legacy session route seal.");
        if (entry.CanonicalRelocationSeal is { } installed
            && installed != request)
            throw new InvalidDataException(
                "A command 42 retry changed fields for the active seal.");
        if (entry.CanonicalRelocationSealResult is { } installedResult)
        {
            drain = null;
            return installedResult;
        }

        var signal = entry.ActiveFrames == 0
            ? null
            : entry.DrainSignal
              ?? new TaskCompletionSource(
                  TaskCreationOptions.RunContinuationsAsynchronously);
        _entries[key] = entry with
        {
            CanonicalRelocationSeal = request,
            AppliedCanonicalRelocationRoute = null,
            DrainSignal = signal
        };
        ArmCanonicalSealTimeout(key, request);
        drain = signal?.Task;
        return null;
    }

    internal async ValueTask<bool> RouteCanonicalAsync(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord request,
            ZLinkSessionRelocationAuthenticatedRoute authenticatedRoute)
    {
        TaskCompletionSource? routeAvailableSignal = null;
        List<ZLinkSessionOutboundCapability> retained = [];
        var deliverRetained = false;
        var routed = await _lane.RunAsync(() =>
        {
            if (_timedOutCanonicalSeals.Contains(
                    CanonicalRelocationKey.From(request)))
            {
                LogLateSessionRouteUpdate(request);
                return false;
            }
            if (!TryFindCanonicalBinding(
                    request.Actor,
                    request.Session,
                    out var key,
                    out var entry))
            {
                LogLateSessionRouteUpdate(request);
                return false;
            }

            if (entry.AppliedCanonicalRelocationRoute is { } applied)
            {
                LogLateSessionRouteUpdate(request);
                return applied == request;
            }

            if (entry.CanonicalRelocationSeal is not { } seal)
            {
                LogLateSessionRouteUpdate(request);
                return false;
            }
            if (seal.RelocationId != request.RelocationId
                || seal.Coordinator != request.Coordinator
                || seal.Actor.Actor != request.Actor
                || seal.Session != request.Session)
            {
                LogLateSessionRouteUpdate(request);
                return false;
            }
            if (entry.CanonicalRelocationSealResult is null)
                throw new InvalidDataException(
                    "Command 44 arrived before the exact command 43 terminal.");

            if (request.Route.Action
                == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit)
            {
                if (request.Route.PreviousAuthorityOwnerGeneration
                    != seal.Actor.AuthorityOwnerGeneration
                    || authenticatedRoute.NodeRid
                    != request.Route.TargetNodeRid
                    || authenticatedRoute.NodeGeneration
                    != request.Route.TargetNodeGeneration
                    || authenticatedRoute.AuthorityOwnerGeneration
                    != request.Route.TargetAuthorityOwnerGeneration
                    || string.IsNullOrWhiteSpace(
                        authenticatedRoute.MeshName))
                    return false;

                var targetActor = new ActorRef(
                    request.Actor.ActorId,
                    request.Actor.ObjectGeneration,
                    authenticatedRoute.MeshName,
                    request.Route.TargetNodeRid);
                var targetOwnerLeaseGeneration =
                    authenticatedRoute.OwnerLeaseGeneration;
                if (targetOwnerLeaseGeneration == 0
                    && _outbound.TryGetValue(key, out var provenOutbound)
                    && provenOutbound.PendingTenureProof is { } proven
                    && MatchesCanonicalOutboundProof(
                        entry,
                        key,
                        request,
                        authenticatedRoute,
                        proven))
                    targetOwnerLeaseGeneration =
                        proven.Tenure.OwnerLeaseGeneration;
                if (!ZLinkSessionBindingRoute.TryCreateRelocated(
                        targetActor,
                        authenticatedRoute.MeshName,
                        request.Route.TargetNodeGeneration,
                        request.Route.TargetAuthorityOwnerGeneration,
                        targetOwnerLeaseGeneration,
                        out var targetRoute))
                    throw new InvalidDataException(
                        "Command 44 target route is invalid.");
                CancelCanonicalSealTimeout(key);
                _entries[key] = entry with
                {
                    Route = targetRoute,
                    CanonicalRelocationSeal = null,
                    CanonicalRelocationSealResult = null,
                    AppliedCanonicalRelocationRoute = request,
                    DrainSignal = null,
                    RouteAvailableSignal = null
                };
                if (_outbound.TryGetValue(key, out var outbound))
                {
                    var targetTenure = new ZLinkSessionOutboundTenure(
                        request.Actor.ActorId,
                        request.Actor.ObjectGeneration,
                        authenticatedRoute.MeshName,
                        request.Route.TargetNodeRid,
                        request.Route.TargetNodeGeneration,
                        request.Route.TargetAuthorityOwnerGeneration,
                        targetOwnerLeaseGeneration,
                        entry.BindingToken,
                        entry.BindingGeneration,
                        entry.SessionOwnerNodeGeneration,
                        entry.ActorRef.SessionRid);
                    deliverRetained = outbound.PendingTenureProof is { } proof
                                      && proof.Tenure == targetTenure;
                    retained = RemoveOutbound(key);
                }
            }
            else
            {
                if (request.Route.CurrentAuthorityOwnerGeneration
                    != entry.AuthorityOwnerGeneration
                    || authenticatedRoute.NodeRid
                    != seal.Coordinator.NodeRid
                    || authenticatedRoute.NodeGeneration
                    != seal.Coordinator.NodeGeneration
                    || authenticatedRoute.AuthorityOwnerGeneration
                    != seal.Actor.AuthorityOwnerGeneration
                    || authenticatedRoute.OwnerLeaseGeneration
                    != seal.Actor.OwnerLeaseGeneration
                    || !string.Equals(
                        authenticatedRoute.MeshName,
                        entry.MeshName,
                        StringComparison.Ordinal))
                    return false;
                CancelCanonicalSealTimeout(key);
                _entries[key] = entry with
                {
                    CanonicalRelocationSeal = null,
                    CanonicalRelocationSealResult = null,
                    AppliedCanonicalRelocationRoute = request,
                    DrainSignal = null,
                    RouteAvailableSignal = null
                };
                retained = RemoveOutbound(key);
            }
            routeAvailableSignal = entry.RouteAvailableSignal;
            return true;
        }).ConfigureAwait(false);
        if (!routed)
            return false;
        routeAvailableSignal?.TrySetResult();
        SettleOutbound(retained, deliverRetained);
        return true;
    }

    internal ValueTask<bool> IsCanonicalRouteAppliedAsync(
        ZLinkServiceWireCodec.SessionRelocationRouteRecord request,
        ZLinkSessionRelocationAuthenticatedRoute authenticatedCandidate) =>
        _lane.RunAsync(() =>
        {
            if (!TryFindCanonicalBinding(
                    request.Actor,
                    request.Session,
                    out _,
                    out var entry)
                || entry.AppliedCanonicalRelocationRoute != request)
                return false;

            var expectedNode = request.Route.Action
                == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                    ? request.Route.TargetNodeRid
                    : request.Coordinator.NodeRid;
            var expectedGeneration = request.Route.Action
                == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                    ? request.Route.TargetNodeGeneration
                    : request.Coordinator.NodeGeneration;
            var expectedAuthority = request.Route.Action
                == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                    ? request.Route.TargetAuthorityOwnerGeneration
                    : request.Route.CurrentAuthorityOwnerGeneration;
            if (authenticatedCandidate.NodeRid != expectedNode
                || authenticatedCandidate.NodeGeneration != expectedGeneration
                || authenticatedCandidate.AuthorityOwnerGeneration
                   != expectedAuthority)
                throw new InvalidDataException(
                    "A command 44 retry changed its authenticated route fingerprint.");
            return true;
        });

    private bool TryFindCanonicalBinding(
        ZLinkServiceWireCodec.SessionActorIdentityRecord actor,
        ZLinkServiceWireCodec.SessionOwnerFenceRecord session,
        out ZLinkSessionBindingKey key,
        out ZLinkSessionBindingEntry entry)
    {
        foreach (var candidate in _entries)
        {
            var value = candidate.Value;
            if (candidate.Key.ActorId.Value == actor.ActorId
                && value.ObjectGeneration == actor.ObjectGeneration
                && value.ActorRef.SessionRid == session.SessionRid
                && value.BindingGeneration == session.BindingGeneration
                && value.SessionOwnerNodeRid == session.SessionOwnerNodeRid
                && value.SessionOwnerNodeGeneration
                == session.SessionOwnerNodeGeneration
                && string.Equals(
                    value.SessionOwnerId,
                    session.SessionOwnerId,
                    StringComparison.Ordinal)
                && value.SessionOwnerLeaseGeneration
                == session.SessionOwnerLeaseGeneration)
            {
                key = candidate.Key;
                entry = value;
                return true;
            }
        }
        key = default;
        entry = null!;
        return false;
    }

    private static bool MatchesCanonicalActorRoute(
        ZLinkSessionBindingEntry entry,
        ZLinkServiceWireCodec.SessionActorRouteFenceRecord actor) =>
        entry.ObjectGeneration == actor.Actor.ObjectGeneration
        && string.Equals(
            entry.ActorRef.ActorId,
            actor.Actor.ActorId,
            StringComparison.Ordinal)
        && entry.Route.Ref.NodeRid == actor.TargetNodeRid
        && entry.TargetNodeGeneration == actor.TargetNodeGeneration
        && entry.AuthorityOwnerGeneration == actor.AuthorityOwnerGeneration
        && entry.OwnerLeaseGeneration == actor.OwnerLeaseGeneration;

    public async ValueTask<ZLinkSessionRouteSealResult> SealRouteAsync(
        ZLinkSessionRouteSeal request,
        CancellationToken cancellationToken)
    {
        //  Receiving side of the route seal. Paired with route_control_sent on
        //  the requester so a stalled seal shows which side never moved.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"route_seal_received actor={request.ActorId}");
        Task? drain = null;
        ulong acceptedHighWater = 0;
        var immediate = await _lane.RunAsync(() =>
                PrepareRouteSeal(
                    request,
                    out drain,
                    out acceptedHighWater))
            .ConfigureAwait(false);
        if (immediate is { } completed)
            return completed;
        if (drain is not null)
            await drain.WaitAsync(cancellationToken).ConfigureAwait(false);
        return await _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(
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
        }).ConfigureAwait(false);
    }

    private ZLinkSessionRouteSealResult? PrepareRouteSeal(
        ZLinkSessionRouteSeal request,
        out Task? drain,
        out ulong acceptedHighWater)
    {
        drain = null;
        acceptedHighWater = 0;
        var key = ZLinkSessionBindingKey.FromBoundary(
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
            ZLinkMeshName.FromBoundary(request.MeshName, nameof(request.MeshName)),
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
        return null;
    }

    public async ValueTask<bool> AbortRouteSealAsync(ZLinkSessionRouteSeal request)
    {
        TaskCompletionSource? routeAvailableSignal = null;
        var aborted = await _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(
                request.ActorId,
                request.BindingToken);
            if (!_entries.TryGetValue(key, out var entry)
                || entry.BindingGeneration != request.BindingGeneration
                || !entry.Route.MatchesFence(
                    request.ActorId,
                    request.ObjectGeneration,
                    request.AuthorityOwnerGeneration,
                    ZLinkMeshName.FromBoundary(request.MeshName, nameof(request.MeshName)),
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
            return true;
        }).ConfigureAwait(false);
        if (!aborted)
            return false;
        routeAvailableSignal?.TrySetResult();
        return true;
    }

    public async ValueTask<bool> UnsealCommittedRouteAsync(
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
        var unsealed = await _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(
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
            return true;
        }).ConfigureAwait(false);
        if (!unsealed)
            return false;
        routeAvailableSignal?.TrySetResult();
        return true;
    }

    public ValueTask<ZLinkSessionRouteCommitResult> CommitRouteAsync(
        ZLinkSessionRouteCommit request) =>
        _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(
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
                    ZLinkMeshName.FromBoundary(
                        request.PreviousMeshName,
                        nameof(request.PreviousMeshName)),
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
        });

    public ValueTask<ZLinkSessionBindingEntry?> GetBindingAsync(
        string actorId,
        string bindingToken) =>
        _lane.RunAsync<ZLinkSessionBindingEntry?>(() =>
        {
            return _entries.TryGetValue(
                ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken),
                out var entry)
                ? entry
                : null;
        });

    public ValueTask<ZLinkSessionBindingRoute?> GetRouteAsync(
        string actorId,
        string bindingToken,
        ZLinkSessionActor actorRef) =>
        _lane.RunAsync<ZLinkSessionBindingRoute?>(() =>
        {
            if (_entries.TryGetValue(
                    ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken),
                    out var entry)
                && ReferenceEquals(entry.ActorRef, actorRef))
            {
                return entry.Route;
            }
            return null;
        });

    public ValueTask UnbindAsync(
        string actorId,
        ZLinkSessionContext context,
        string bindingToken) =>
        _lane.RunAsync(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
            if (_entries.TryGetValue(key, out var existing)
                && ReferenceEquals(existing.Context, context)
                && string.Equals(existing.BindingToken, bindingToken, StringComparison.Ordinal))
            {
                CancelCanonicalSealTimeout(key);
                _entries.Remove(key);
                SettleOutbound(RemoveOutbound(key), deliver: false);
                existing.DrainSignal?.TrySetResult();
                existing.RouteAvailableSignal?.TrySetResult();
            }
        });

    public ValueTask<ZLinkSessionContext?> GetContextAsync(
        string actorId,
        string bindingToken) =>
        _lane.RunAsync<ZLinkSessionContext?>(() =>
        {
            var key = ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
            if (_entries.TryGetValue(key, out var entry))
            {
                return entry.Context;
            }

            return null;
        });

    public ValueTask<ZLinkSessionContext?> GetContextByActorIdAsync(
        string actorId) =>
        _lane.RunAsync<ZLinkSessionContext?>(() =>
        {
            var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
            foreach (var entry in _entries)
                if (entry.Key.ActorId == actorKey)
                {
                    return entry.Value.Context;
                }

            return null;
        });

    public ValueTask<ZLinkSessionBindingEntry?> GetEntryByActorIdAsync(
        string actorId) =>
        _lane.RunAsync<ZLinkSessionBindingEntry?>(() =>
        {
            var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
            foreach (var candidate in _entries)
                if (candidate.Key.ActorId == actorKey)
                {
                    return candidate.Value;
                }
            return null;
        });

    public ValueTask<ZLinkSessionBindingEntry?> GetExactRetiredBindingAsync(
        string actorId,
        RoutingId sessionOwnerNodeRid,
        RoutingId sessionRid,
        ulong sessionOwnerNodeGeneration,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration,
        ulong bindingGeneration) =>
        _lane.RunAsync(() => GetExactRetiredBinding(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            sessionOwnerNodeRid,
            sessionRid,
            sessionOwnerNodeGeneration,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            bindingGeneration));

    internal ValueTask<ZLinkSessionBindingEntry?> GetExactRetiredBindingAsync(
        ZLinkActorId actorId,
        RoutingId sessionOwnerNodeRid,
        RoutingId sessionRid,
        ulong sessionOwnerNodeGeneration,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration,
        ulong bindingGeneration) =>
        _lane.RunAsync(() => GetExactRetiredBinding(
            actorId,
            sessionOwnerNodeRid,
            sessionRid,
            sessionOwnerNodeGeneration,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            bindingGeneration));

    private ZLinkSessionBindingEntry? GetExactRetiredBinding(
        ZLinkActorId actorId,
        RoutingId sessionOwnerNodeRid,
        RoutingId sessionRid,
        ulong sessionOwnerNodeGeneration,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration,
        ulong bindingGeneration)
    {
        foreach (var candidate in _entries)
        {
            var value = candidate.Value;
            if (candidate.Key.ActorId != actorId
                || value.ActorRef.SessionRid != sessionRid
                || value.BindingGeneration != bindingGeneration
                || value.SessionOwnerNodeRid != sessionOwnerNodeRid
                || value.SessionOwnerNodeGeneration
                   != sessionOwnerNodeGeneration
                || !string.Equals(
                    value.SessionOwnerId,
                    sessionOwnerId,
                    StringComparison.Ordinal)
                || value.SessionOwnerLeaseGeneration
                   != sessionOwnerLeaseGeneration)
                continue;
            return value;
        }
        return null;
    }

    public ValueTask<IReadOnlyCollection<IZLinkSessionActor>> SnapshotActorsAsync(
        ZLinkSessionContext context) =>
        _lane.RunAsync<IReadOnlyCollection<IZLinkSessionActor>>(() =>
        {
            return _entries.Values
                .Where(entry => ReferenceEquals(entry.Context, context))
                .Select(static entry => (IZLinkSessionActor)entry.ActorRef)
                .ToArray();
        });

    public ValueTask<ZLinkSessionActor?> FindActorAsync(
        ZLinkSessionContext context,
        string actorId) =>
        _lane.RunAsync(() =>
        {
            return _entries.Values
                .Where(entry => ReferenceEquals(entry.Context, context))
                .FirstOrDefault(entry => string.Equals(
                    entry.ActorRef.ActorId,
                    actorId,
                    StringComparison.Ordinal))
                ?.ActorRef;
        });

    public ValueTask ResetGenerationAsync() =>
        _lane.RunAsync(() =>
        {
            foreach (var timeout in _canonicalSealTimeouts.Values)
                timeout.Cancellation.Cancel();
            _canonicalSealTimeouts.Clear();
            _timedOutCanonicalSeals.Clear();
            _timedOutCanonicalSealOrder.Clear();
            foreach (var entry in _entries.Values)
            {
                entry.DrainSignal?.TrySetResult();
                entry.RouteAvailableSignal?.TrySetResult();
            }
            foreach (var outbound in _outbound.Values)
                SettleOutbound(outbound.Retained, deliver: false);
            _outbound.Clear();
            _entries.Clear();
            _tombstones.Clear();
        });
}
