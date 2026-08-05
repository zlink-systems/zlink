using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Executes the connect/disconnect decisions of the reconciler. The channel
/// runtime implements this over core socket calls; stores never touch
/// sockets. Connect failures are the executor's concern (retry backoff) and
/// are never a reason to remove a location row.
/// </summary>
internal interface IZLinkAutoConnectExecutor
{
    bool Connect(ZLinkAutoConnectTarget target);

    bool Disconnect(ZLinkAutoConnectTarget target);
}

/// <summary>
/// Per-mesh reconcile loop state machine. Reads the mesh descriptor
/// snapshot through the resolver, computes the desired target set, and
/// diffs it against the active set. Store failures are fail-static: the
/// last desired set stays, no diff runs, and after recovery the local
/// descriptor is re-published first and disconnects wait one heartbeat
/// interval so peers that have not re-registered yet are not cut off in one
/// sweep. A draining descriptor is excluded from new selections but an
/// already-active connection to it stays up (40-location-runtime §5).
/// </summary>
internal sealed class ZLinkAutoConnectReconciler
{
    private readonly ZLinkAutoConnectLocal _local;
    private ZLinkMeshNodeDescriptor? _localRow;
    private readonly ZLinkLocationRuntime _runtime;
    private readonly IZLinkMeshNodeLocationResolver _peers;
    private readonly IZLinkAutoConnectExecutor _executor;
    private readonly ZLinkLocationOptions _options;
    private readonly TimeProvider _time;
    private readonly SemaphoreSlim _reconcileGate = new(1, 1);
    private readonly Dictionary<string, ZLinkAutoConnectTarget> _active = new(StringComparer.Ordinal);
    private Dictionary<string, ZLinkAutoConnectTarget> _lastDesired = new(StringComparer.Ordinal);
    private volatile Dictionary<string, ZLinkRouteMeshTargetClassification>?
        _meshTargets;
    private volatile ZLinkRouteMeshPeerIdentity[]? _meshPeers;
    private volatile HashSet<string> _retainedMemberRids = new(StringComparer.Ordinal);
    private readonly bool _retainRemovedMembers;
    private ulong _localGeneration;
    private ulong _localRevision;
    private bool _localPublished;
    private bool _storeFailed;
    private long? _storeFailureStartedAt;
    private long _recoveryDeferUntil;
    private bool _ownerCleanupStarted;
    private long _discoveredPeerCount;
    private long _pendingLocalWeight = -1;
    private long _pendingPlacementWeight = -1;
    private long _pendingActivationConcurrency = -1;
    private readonly System.Collections.Concurrent.ConcurrentDictionary<string, int>
        _pendingChannelWeights = new(StringComparer.Ordinal);

    /// <summary>
    /// <paramref name="localRow"/> is null for a dial-only capability that
    /// has no advertisable descriptor (a client dealer or a subscriber): it
    /// cannot be keyed, but its reconcile loop still dials remote
    /// descriptors.
    /// </summary>
    internal ZLinkAutoConnectReconciler(
        ZLinkAutoConnectLocal local,
        ZLinkMeshNodeDescriptor? localRow,
        ZLinkLocationRuntime runtime,
        IZLinkMeshNodeLocationResolver peers,
        IZLinkAutoConnectExecutor executor,
        ZLinkLocationOptions options,
        TimeProvider? timeProvider = null,
        bool retainRemovedMembers = false,
        bool initiallyPublished = false,
        ulong initialStoreGeneration = 0)
    {
        _local = local;
        _localRow = localRow;
        _localRevision = localRow?.DescriptorRevision ?? 0;
        _runtime = runtime;
        _peers = peers;
        _executor = executor;
        _options = options;
        _time = timeProvider ?? TimeProvider.System;
        _retainRemovedMembers = retainRemovedMembers;
        _localPublished = initiallyPublished;
        _localGeneration = initialStoreGeneration;
    }

    internal IReadOnlyCollection<ZLinkAutoConnectTarget> ActiveTargets => _active.Values;

    /// <summary>
    /// Classifies a Node-direct target from the last complete descriptor
    /// snapshot. Object role is retained because an Object Client can still
    /// have a required physical connection while remaining ineligible as an
    /// application Node-direct target.
    /// </summary>
    internal ZLinkRouteMeshTargetClassification ClassifyTarget(
        RoutingId nodeRid)
    {
        if (_meshTargets is not { } targets)
            return ZLinkRouteMeshTargetClassification.Unknown;
        return targets.GetValueOrDefault(
            nodeRid.ToHex(),
            ZLinkRouteMeshTargetClassification.Unknown);
    }

    internal IReadOnlyList<ZLinkRouteMeshPeerIdentity>? CompleteMeshPeers()
    {
        if (_storeFailed) return null;
        return _meshPeers;
    }

    internal bool HasRetainedPeer(RoutingId nodeRid) =>
        _retainRemovedMembers && _retainedMemberRids.Contains(nodeRid.ToHex());

    /// <summary>True while the last tick could not read the store. The loop
    /// must not let a change stamp skip ticks in this state.</summary>
    internal bool StoreFailed => _storeFailed;

    internal void SetLocalWeight(uint weight) => Volatile.Write(ref _pendingLocalWeight, weight);

    internal void SetLocalChannelWeight(string channelName, int weight) =>
        _pendingChannelWeights[channelName] = weight;

    internal void SetLocalPlacementWeight(int weight) =>
        Volatile.Write(ref _pendingPlacementWeight, weight);

    internal void SetLocalActivationConcurrency(int active) =>
        Volatile.Write(ref _pendingActivationConcurrency, active);

    internal ValueTask<bool> SetLocalWeightAsync(
        uint weight,
        CancellationToken cancellationToken = default) =>
        PublishLocalMutationAsync(
            row => WithWeight(row, weight),
            cancellationToken);

    internal ValueTask<bool> SetAllLocalChannelWeightsAsync(
        uint weight,
        CancellationToken cancellationToken = default) =>
        PublishLocalMutationAsync(
            row => WithAllChannelWeights(row, weight),
            cancellationToken);

    internal bool HasPendingTargets
    {
        get
        {
            var pendingWeight = Volatile.Read(ref _pendingLocalWeight);
            if (_localRow is { } localRow
                && pendingWeight >= 0
                && WeightOf(localRow) != (int)pendingWeight)
                return true;
            var pendingPlacementWeight =
                Volatile.Read(ref _pendingPlacementWeight);
            if (_localRow is { } placementRow
                && pendingPlacementWeight >= 0
                && placementRow.PlacementWeight != pendingPlacementWeight)
                return true;
            var pendingActivationConcurrency =
                Volatile.Read(ref _pendingActivationConcurrency);
            if (_localRow is { } activationRow
                && pendingActivationConcurrency >= 0
                && activationRow.ActivationConcurrency.Active
                    != pendingActivationConcurrency)
                return true;
            if (_localRow is { } channelRow
                && _pendingChannelWeights.Any(entry =>
                    !channelRow.ChannelWeights.TryGetValue(entry.Key, out var current)
                    || current != entry.Value))
                return true;

            foreach (var (key, desired) in _lastDesired)
            {
                if (!_active.TryGetValue(key, out var active))
                {
                    if (!desired.Draining) return true;
                    continue;
                }

                if (RequiresTargetRefresh(active, desired)) return true;
            }

            return false;
        }
    }

    internal async ValueTask<bool> MarkDrainingAsync(
        CancellationToken cancellationToken = default)
        => await PublishLocalMutationAsync(
            row => row with { State = ZLinkFrameworkRuntimeState.Draining },
            cancellationToken).ConfigureAwait(false);

    internal async ValueTask<bool> MarkRetiringAsync(
        CancellationToken cancellationToken = default)
        => await PublishLocalMutationAsync(
            row => row with { State = ZLinkFrameworkRuntimeState.Relocating },
            cancellationToken).ConfigureAwait(false);

    internal async ValueTask<bool> MarkServingAsync(
        CancellationToken cancellationToken = default)
        => await PublishLocalMutationAsync(
            row => row.State == ZLinkFrameworkRuntimeState.Serving
                ? row
                : row with { State = ZLinkFrameworkRuntimeState.Serving },
            cancellationToken).ConfigureAwait(false);

    private async ValueTask<bool> PublishLocalMutationAsync(
        Func<ZLinkMeshNodeDescriptor, ZLinkMeshNodeDescriptor> mutation,
        CancellationToken cancellationToken)
    {
        await _reconcileGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_localRow is null) return true;
            // Weight and drain changes increment the descriptor revision so
            // readers on the same lifecycle generation apply the newest
            // snapshot only (40-location-runtime §2.1).
            _localRow = mutation(_localRow) with { DescriptorRevision = ++_localRevision };
            if (!_localPublished)
                await PublishLocalAsync(cancellationToken).ConfigureAwait(false);
            if (!_localPublished || _localGeneration == 0) return false;

            var result = await _runtime.WriteDescriptorAsync(
                    _localRow,
                    ZLinkLocationWriteIntent.Renew,
                    cancellationToken)
                .ConfigureAwait(false);
            _localPublished = result.Status == ZLinkLocationWriteStatus.Stored;
            if (_localPublished) _localGeneration = result.Generation;
            return _localPublished;
        }
        finally
        {
            _reconcileGate.Release();
        }
    }

    internal async ValueTask FreezeOwnerWritesAsync(CancellationToken cancellationToken)
    {
        await _reconcileGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            _ownerCleanupStarted = true;
        }
        finally
        {
            _reconcileGate.Release();
        }
    }

    internal async ValueTask TickAsync(CancellationToken cancellationToken = default)
    {
        await _reconcileGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await TickCoreAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _reconcileGate.Release();
        }
    }

    internal async ValueTask NoteStoreFailureAsync(
        CancellationToken cancellationToken = default)
    {
        await _reconcileGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            EnterStoreFailure();
        }
        finally
        {
            _reconcileGate.Release();
        }
    }

    private async ValueTask TickCoreAsync(CancellationToken cancellationToken)
    {
        if (_ownerCleanupStarted) return;
        // A read that began before a store outage can complete after Redis
        // resumes without ever throwing. The owner heartbeat is the shared
        // recovery authority: while it is unhealthy, even a successful list
        // result must not drive a disconnect diff from an incomplete lease
        // snapshot.
        if (!_runtime.GetHealthSnapshot().Healthy)
        {
            EnterStoreFailure();
            return;
        }

        var pendingWeight = Volatile.Read(ref _pendingLocalWeight);
        if (_localRow is { } localRow
            && pendingWeight >= 0
            && WeightOf(localRow) != (int)pendingWeight)
        {
            _localRow = WithWeight(localRow, (uint)pendingWeight)
                with { DescriptorRevision = ++_localRevision };
            _localPublished = false;
        }
        if (_localRow is { } channelRow)
        {
            var pendingChannels = _pendingChannelWeights.ToArray();
            if (pendingChannels.Any(entry =>
                    !channelRow.ChannelWeights.TryGetValue(entry.Key, out var current)
                    || current != entry.Value))
            {
                _localRow = WithChannelWeights(channelRow, pendingChannels)
                    with { DescriptorRevision = ++_localRevision };
                _localPublished = false;
            }
        }
        var pendingPlacementWeight =
            Volatile.Read(ref _pendingPlacementWeight);
        if (_localRow is { } placementRow
            && pendingPlacementWeight >= 0
            && placementRow.PlacementWeight != pendingPlacementWeight)
        {
            _localRow = placementRow with
            {
                PlacementWeight = checked((int)pendingPlacementWeight),
                DescriptorRevision = ++_localRevision
            };
            _localPublished = false;
        }
        var pendingActivationConcurrency =
            Volatile.Read(ref _pendingActivationConcurrency);
        if (_localRow is { } activationRow
            && pendingActivationConcurrency >= 0
            && activationRow.ActivationConcurrency.Active
                != pendingActivationConcurrency)
        {
            _localRow = activationRow with
            {
                ActivationConcurrency = activationRow.ActivationConcurrency with
                {
                    Active = checked((int)pendingActivationConcurrency)
                },
                DescriptorRevision = ++_localRevision
            };
            _localPublished = false;
        }
        // Publish (or re-publish after recovery) the local descriptor before
        // reading the list, so peers observing the store during our
        // recovery window can already see us.
        IReadOnlyList<ZLinkMeshNodeDescriptor> rows;
        try
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            deadline.CancelAfter(_options.OwnerLeaseRenewTimeout);
            await PublishLocalAsync(deadline.Token).ConfigureAwait(false);
            rows = await _peers.ListLiveMeshNodesAsync(_local.MeshName, deadline.Token)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"autoconnect_snapshot local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                + $"mesh={_local.MeshName} rows={rows.Count} "
                + $"rids={string.Join(',', rows.Select(static row => row.Rid.ToString()))}");
            if (!_runtime.GetHealthSnapshot().Healthy)
            {
                EnterStoreFailure();
                return;
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (ZLinkFrameworkException error)
            when (error.Kind == ZLinkFrameworkErrorKind.AlreadyExists)
        {
            // A conflicting local identity is a deterministic startup
            // configuration failure, not a transient store outage.
            throw;
        }
        catch (Exception exception)
        {
            // Fail-static: keep the last desired set, compute no diff, and
            // keep already-ready connections alive. While the store is
            // unreachable the loop cannot accept expanded desired sets, so
            // no new outbound connects are started after the failure.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"autoconnect_tick_failed local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                + $"mesh={_local.MeshName} exception={exception.GetType().Name} "
                + $"message={exception.Message}");
            EnterStoreFailure();
            return;
        }

        if (_storeFailed)
        {
            // First successful read after an outage: defer disconnects for
            // one heartbeat interval so other nodes get time to re-register.
            _storeFailed = false;
            _storeFailureStartedAt = null;
            _recoveryDeferUntil = _time.GetTimestamp()
                + (long)(
                    _options.OwnerLeaseRenewInterval.TotalSeconds
                    * _time.TimestampFrequency);
        }

        var desired = ZLinkAutoConnectPlanner.ComputeDesired(_local, rows);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"autoconnect_desired local={_local.NodeRid?.ToString() ?? "<unknown>"} "
            + $"mesh={_local.MeshName} count={desired.Count} "
            + $"targets={string.Join(',', desired.Values.Select(static target =>
                $"{target.NodeRid}:{(target.InitiatesConnection ? "dial" : "await")}"))}");
        Volatile.Write(
            ref _discoveredPeerCount,
            ZLinkAutoConnectPlanner.CountDiscoveredPeers(_local, rows));
        // A rolling RID replacement can leave both descriptors visible for one
        // snapshot. One Core endpoint is still one transport candidate, so
        // keep only the newest deterministic owner before diffing. Without
        // this projection the old descriptor can reclaim the endpoint on the
        // tick after a deferred handover and oscillate with its replacement.
        var connectableDesired = SelectEndpointWinners(desired);
        _lastDesired = connectableDesired;
        // Membership snapshot for fail-fast target classification on the
        // send path (known peer vs unknown node). This is the full mesh
        // view, NOT the desired dial set: the pairwise initiator keeps
        // peers that dial us out of `desired`, yet they are reachable
        // rid-addressed targets. Fail-static: a store outage keeps the
        // last snapshot because the tick returns before this point.
        var members = new HashSet<string>(StringComparer.Ordinal);
        var targets =
            new Dictionary<string, ZLinkRouteMeshTargetClassification>(
                StringComparer.Ordinal);
        foreach (var row in rows)
        {
            if (row.Rid is not { Size: > 0 } rowRid)
                continue;
            var key = rowRid.ToHex();
            members.Add(key);
            if (_local.NodeRid is { } localRid && rowRid == localRid)
                continue;
            targets[key] =
                row.ObjectRole == ZLinkMeshNodeObjectRole.Client
                    ? ZLinkRouteMeshTargetClassification.ObjectClientTarget
                    : ZLinkRouteMeshTargetClassification.RequiredNotConnected;
        }

        _meshTargets = targets;
        _meshPeers = rows
            .Where(row => row.Rid is { Size: > 0 } rowRid
                          && (_local.NodeRid is not { } localRid
                              || rowRid != localRid))
            .Select(static row => new ZLinkRouteMeshPeerIdentity(
                row.Rid!,
                row.LifecycleGeneration,
                row.State is ZLinkFrameworkRuntimeState.Relocating
                    or ZLinkFrameworkRuntimeState.Draining))
            .ToArray();
        if (_retainRemovedMembers)
        {
            var retained = new HashSet<string>(_retainedMemberRids, StringComparer.Ordinal);
            retained.UnionWith(members);
            _retainedMemberRids = retained;
        }

        foreach (var (key, target) in connectableDesired)
        {
            if (!_active.TryGetValue(key, out var current))
            {
                // A draining descriptor is not selected for new connections.
                if (target.Draining) continue;
                if (!ReleaseEndpointConflicts(target, out _))
                    continue;
                var accepted = _executor.Connect(target);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"autoconnect_add local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                    + $"target={target.NodeRid} endpoint={target.Endpoint} accepted={accepted}");
                if (accepted)
                {
                    _active[key] = target;
                }
                continue;
            }

            if (RequiresConnectionHandover(current, target))
            {
                // An endpoint change needs a new transport connection.
                var disconnected = _executor.Disconnect(current);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"autoconnect_handover local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                    + $"old={current.NodeRid}@{current.Endpoint} new={target.NodeRid}@{target.Endpoint} "
                    + $"disconnect={disconnected}");
                if (!disconnected) continue;
                _active.Remove(key);
                var connected = _executor.Connect(target);
                if (connected)
                {
                    _active[key] = target;
                }
            }
            else if (OwnerChanged(current, target) || current.Draining != target.Draining)
            {
                // A restarted process can reclaim the same endpoint under a new owner.
                // The transport already reconnects that broken endpoint. Tearing it down
                // again here races the reconnect and can leave a stale pipe beside the
                // replacement connection, so only refresh the reconciler's metadata.
                _active[key] = target;
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"autoconnect_refresh local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                    + $"target={target.NodeRid} endpoint={target.Endpoint} owner_changed={OwnerChanged(current, target)} "
                    + $"draining={target.Draining}");
            }
        }

        if (_time.GetTimestamp() >= _recoveryDeferUntil)
        {
            var toRemove = _active.Keys
                .Where(key => !connectableDesired.ContainsKey(key))
                .ToArray();
            foreach (var key in toRemove)
            {
                var target = _active[key];
                var disconnected = _executor.Disconnect(target);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"autoconnect_remove local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                    + $"target={target.NodeRid} endpoint={target.Endpoint} disconnected={disconnected}");
                if (disconnected)
                {
                    _active.Remove(key);
                }
            }
        }

    }

    private bool ReleaseEndpointConflicts(
        ZLinkAutoConnectTarget target,
        out bool endpointReleased)
    {
        endpointReleased = false;
        // A restarted process may publish a new RID before the old lease row
        // expires. The endpoint is still one Core transport candidate, so an
        // old RID cannot retain the framework's auto-connect claim while the
        // new descriptor is admitted. Release only a different active target
        // on the same endpoint; the same-RID owner refresh remains transport
        // managed and does not trigger a second dial.
        var conflicts = _active
            .Where(entry =>
                !string.Equals(entry.Key, target.TargetKey, StringComparison.Ordinal)
                && string.Equals(
                    entry.Value.Endpoint,
                    target.Endpoint,
                    StringComparison.Ordinal))
            .ToArray();
        if (conflicts.Any(entry => !SupersedesEndpointTarget(target, entry.Value)))
            return false;

        foreach (var (key, current) in conflicts)
        {
            var disconnected = _executor.Disconnect(current);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"autoconnect_endpoint_handover local={_local.NodeRid?.ToString() ?? "<unknown>"} "
                + $"old={current.NodeRid}@{current.Endpoint} new={target.NodeRid}@{target.Endpoint} "
                + $"disconnect={disconnected}");
            if (!disconnected) return false;
            _active.Remove(key);
            endpointReleased = true;
        }

        return true;
    }

    private static Dictionary<string, ZLinkAutoConnectTarget> SelectEndpointWinners(
        IReadOnlyDictionary<string, ZLinkAutoConnectTarget> desired)
    {
        var winners = new Dictionary<string, ZLinkAutoConnectTarget>(
            StringComparer.Ordinal);
        foreach (var endpointGroup in desired.Values.GroupBy(
                     static target => target.Endpoint,
                     StringComparer.Ordinal))
        {
            // Keep a draining target only when no serving target currently
            // owns the endpoint. This preserves an already-active draining
            // connection while preventing it from blocking a replacement
            // that is eligible for a new connection.
            var candidates = endpointGroup.Any(static target => !target.Draining)
                ? endpointGroup.Where(static target => !target.Draining)
                : endpointGroup;
            var winner = candidates.Aggregate(
                static (current, candidate) =>
                    SupersedesEndpointTarget(candidate, current)
                        ? candidate
                        : current);
            winners[endpointGroup.Key] = winner;
        }

        return winners.Values.ToDictionary(
            static target => target.TargetKey,
            static target => target,
            StringComparer.Ordinal);
    }

    private static bool SupersedesEndpointTarget(
        ZLinkAutoConnectTarget target,
        ZLinkAutoConnectTarget current)
    {
        var updatedAt = target.UpdatedAt.CompareTo(current.UpdatedAt);
        if (updatedAt != 0) return updatedAt > 0;
        if (target.OwnerLeaseGeneration != current.OwnerLeaseGeneration)
            return target.OwnerLeaseGeneration > current.OwnerLeaseGeneration;
        if (target.LifecycleGeneration != current.LifecycleGeneration)
            return target.LifecycleGeneration > current.LifecycleGeneration;
        // The store normally supplies UpdatedAt and a lease generation. Keep
        // the tie deterministic for test stores or clocks with coarse
        // resolution instead of allowing two RIDs to oscillate per tick.
        return string.CompareOrdinal(target.TargetKey, current.TargetKey) > 0;
    }

    private void EnterStoreFailure()
    {
        _storeFailureStartedAt ??= _time.GetTimestamp();
        _storeFailed = true;
        _localPublished = false;
        RetryPendingTargetsWithinStoreFailureGrace();
    }

    private int WeightOf(ZLinkMeshNodeDescriptor row) =>
        row.ChannelWeights.TryGetValue(_local.MeshName, out var weight) ? weight : -1;

    private ZLinkMeshNodeDescriptor WithWeight(ZLinkMeshNodeDescriptor row, uint weight)
    {
        var weights = new Dictionary<string, int>(row.ChannelWeights, StringComparer.Ordinal)
        {
            [_local.MeshName] = (int)weight
        };
        return row with { ChannelWeights = weights };
    }

    private static ZLinkMeshNodeDescriptor WithChannelWeights(
        ZLinkMeshNodeDescriptor row,
        IReadOnlyList<KeyValuePair<string, int>> updates)
    {
        var weights = new Dictionary<string, int>(
            row.ChannelWeights,
            StringComparer.Ordinal);
        foreach (var update in updates)
            weights[update.Key] = update.Value;
        return row with { ChannelWeights = weights };
    }

    private static ZLinkMeshNodeDescriptor WithAllChannelWeights(
        ZLinkMeshNodeDescriptor row,
        uint weight)
    {
        var weights = row.ChannelWeights.Keys.ToDictionary(
            static channelName => channelName,
            _ => (int)weight,
            StringComparer.Ordinal);
        return row with { ChannelWeights = weights };
    }

    private static bool RequiresTargetRefresh(
        ZLinkAutoConnectTarget current,
        ZLinkAutoConnectTarget target) =>
        RequiresConnectionHandover(current, target)
        || OwnerChanged(current, target)
        || current.OwnerLeaseGeneration != target.OwnerLeaseGeneration
        || current.LifecycleGeneration != target.LifecycleGeneration
        || current.Draining != target.Draining;

    private static bool RequiresConnectionHandover(
        ZLinkAutoConnectTarget current,
        ZLinkAutoConnectTarget target) =>
        !string.Equals(current.Endpoint, target.Endpoint, StringComparison.Ordinal);

    private static bool OwnerChanged(
        ZLinkAutoConnectTarget current,
        ZLinkAutoConnectTarget target) =>
        !string.Equals(current.OwnerId, target.OwnerId, StringComparison.Ordinal);

    private void RetryPendingTargetsWithinStoreFailureGrace()
    {
        if (_storeFailureStartedAt is not { } started
            || _options.StoreFailureGrace <= TimeSpan.Zero
            || _time.GetElapsedTime(started, _time.GetTimestamp()) > _options.StoreFailureGrace)
            return;

        foreach (var (key, target) in _lastDesired)
        {
            if (_active.ContainsKey(key) || target.Draining) continue;
            if (_executor.Connect(target)) _active[key] = target;
        }
    }

    internal async ValueTask ShutdownAsync(CancellationToken cancellationToken = default)
    {
        if (_localRow is not null && _localGeneration > 0)
        {
            await _runtime.RemoveDescriptorForShutdownAsync(
                    LocalKey(),
                    cancellationToken)
                .ConfigureAwait(false);
            _localPublished = false;
            _localGeneration = 0;
        }

        foreach (var target in _active.Values)
        {
            _ = _executor.Disconnect(target);
        }

        _active.Clear();
    }

    private async ValueTask PublishLocalAsync(CancellationToken cancellationToken)
    {
        if (_localRow is null || _localPublished)
        {
            return;
        }

        if (_localGeneration > 0)
        {
            var renewed = await _runtime.WriteDescriptorAsync(
                    _localRow,
                    ZLinkLocationWriteIntent.Renew,
                    cancellationToken)
                .ConfigureAwait(false);
            _localPublished = renewed.Status == ZLinkLocationWriteStatus.Stored;
            return;
        }

        var claim = await _runtime.WriteDescriptorAsync(
            _localRow, ZLinkLocationWriteIntent.NewClaim, cancellationToken)
            .ConfigureAwait(false);
        if (claim.Status == ZLinkLocationWriteStatus.Stored)
        {
            // Store generation tracks the published row revision domain. The
            // owner lease token remains the independent renew/remove fence.
            _localGeneration = claim.Generation;
            _localPublished = true;
            return;
        }

        if (claim.Status == ZLinkLocationWriteStatus.RejectedConflict)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.AlreadyExists,
                $"MeshNode RID '{_localRow.Rid}' is already claimed in mesh "
                + $"'{_localRow.MeshName}'.");
    }

    private ZLinkMeshNodeDescriptorKey LocalKey() =>
        new(_localRow!.MeshName, _localRow.Rid);
}
