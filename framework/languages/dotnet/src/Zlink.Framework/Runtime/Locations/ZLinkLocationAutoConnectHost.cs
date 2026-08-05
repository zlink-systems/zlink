using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkRetiringPublicationRollbackException(
    IReadOnlyList<Exception> failures) : Exception(
    "A partial Retiring publication could not be restored to Serving.",
    failures.Count == 1 ? failures[0] : new AggregateException(failures));

/// <summary>
/// Builds one reconcile loop per auto-connect capability from the framework
/// registration and the started runtime state. Dialing capabilities get an
/// executor over the channel's core socket surface; advertise-only
/// capabilities (publishers) run the same loop with a
/// never-called executor so their peer row is published and removed by the
/// same lifecycle. Core discovery is never involved.
/// </summary>
internal sealed class ZLinkLocationAutoConnectHost : IAsyncDisposable, IZLinkAutoConnectTopologyQuery
{
    private readonly ZLinkLocationRuntime _runtime;
    private readonly IZLinkMeshNodeLocationResolver _peers;
    private readonly ZLinkLocationOptions _options;
    private readonly IZLinkLocationRepository _store;
    private readonly IZLinkLocationWatchStore? _watchStore;
    private readonly ZLinkOwnerLeaseTracker? _leaseTracker;
    private readonly TimeProvider _time;
    private readonly SemaphoreSlim _lifecycleGate = new(1, 1);
    private readonly List<ZLinkAutoConnectLoop> _loops = [];
    private readonly List<ZLinkAutoConnectReconciler> _reconcilers = [];
    private readonly System.Collections.Concurrent.ConcurrentDictionary<string, ZLinkAutoConnectReconciler>
        _routeMeshReconcilers = new(StringComparer.Ordinal);
    private readonly System.Collections.Concurrent.ConcurrentDictionary<
        (ZLinkLocationAutoConnectType Type, string MeshName, ZLinkLocationRole Role),
        ZLinkAutoConnectReconciler> _localReconcilers = new();
    private readonly System.Collections.Concurrent.ConcurrentDictionary<
        (ZLinkLocationAutoConnectType Type, string MeshName, ZLinkLocationRole Role),
        ZLinkAutoConnectLoop> _localLoops = new();
    private readonly object _disposeGate = new();
    private ZLinkClientServerDiscovery? _clientServerDiscovery;
    private ZLinkFanoutDiscovery? _fanoutDiscovery;
    private int _disposed;
    private Task? _disposeTask;

    internal ZLinkLocationAutoConnectHost(
        ZLinkLocationRuntime runtime,
        IZLinkMeshNodeLocationResolver peers,
        ZLinkLocationOptions options,
        IZLinkLocationWatchStore? watchStore = null,
        TimeProvider? timeProvider = null,
        ZLinkOwnerLeaseTracker? leaseTracker = null,
        IZLinkLocationRepository? store = null)
    {
        _runtime = runtime;
        _peers = peers;
        _options = options;
        _store = store ?? runtime.Store;
        _watchStore = watchStore;
        _leaseTracker = leaseTracker;
        _time = timeProvider ?? TimeProvider.System;
    }

    internal async ValueTask StartAsync(
        ZLinkFrameworkComponentState state,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_loops.Count != 0) return;
            var registration = state.Registration;
            if (registration.Channels.Values.Any(static channel =>
                    channel.ClientServerRole is not null))
            {
                _clientServerDiscovery = new ZLinkClientServerDiscovery(
                    _store,
                    _runtime,
                    _options,
                    _leaseTracker);
                await _clientServerDiscovery.StartAsync(state, cancellationToken)
                    .ConfigureAwait(false);
            }

            if (registration.Channels.Values.Any(static channel =>
                    channel.AutoConnectType == ZLinkLocationAutoConnectType.Fanout
                    && (channel.Publisher is not null
                        || channel.Subscriber?.AcquisitionMode
                            == ZLinkPeerAcquisitionMode.AutoConnect)))
            {
                _fanoutDiscovery = new ZLinkFanoutDiscovery(
                    _store,
                    _runtime,
                    _options,
                    _leaseTracker);
                await _fanoutDiscovery.StartAsync(state, cancellationToken)
                    .ConfigureAwait(false);
            }

        foreach (var (name, spot) in registration.SpotNodes)
        {
            if (!state.SpotNodes.TryGetValue(name, out var node) || spot.Router is null) continue;
            if (node.StartupState is not { } startupState) continue;

            var meshName = spot.SpotMeshChannelName ?? spot.SpotNodeName;
            AddLoop(
                ZLinkLocationAutoConnectType.SpotMesh,
                meshName,
                ZLinkLocationRole.Spot,
                startupState.RoutingId,
                startupState.Descriptor.Endpoint,
                (uint)spot.Router.SocketConfig.Weight,
                new SpotRouterExecutor(
                    node,
                    spot.Router.AcquisitionMode == ZLinkPeerAcquisitionMode.AutoConnect),
                retainRemovedMembers:
                    spot.Router.AcquisitionMode == ZLinkPeerAcquisitionMode.Manual,
                // The row carries Core's nonzero lifecycle token verbatim.
                // Admission compares this opaque token for exact equality.
                lifecycleGeneration: node.Node.MeshStatus().LifecycleGeneration,
                objectRole: spot.ObjectRole,
                objectCapabilities: BuildObjectCapabilities(spot),
                applicationVersion: registration.ApplicationVersion,
                maintenanceWave: registration.MaintenanceWave,
                entrySpotId: spot.EntrySpotId,
                placementWeight: spot.PlacementWeight,
                capacity: new ZLinkPlacementCapacity(
                    new ZLinkPopulationCapacity(
                        0,
                        0,
                        spot.ActorLimit),
                    new ZLinkPopulationCapacity(
                        0,
                        0,
                        spot.SpotLimit),
                    BuildSpotTypeCapacities(spot)),
                activationConcurrency: new ZLinkActivationConcurrency(
                    0,
                    spot.ActivationConcurrencyLimit),
                channelWeights: spot.ChannelMemberships
                    .Where(static membership => membership.IsServer)
                    .ToDictionary(
                        static membership => membership.ChannelName,
                        static membership => membership.Weight,
                        StringComparer.Ordinal),
                startupState: startupState);
        }

            try
            {
                foreach (var loop in _loops)
                    await loop.StartAsync(cancellationToken).ConfigureAwait(false);
                foreach (var reconciler in _reconcilers)
                {
                    if (!await reconciler.MarkServingAsync(cancellationToken)
                            .ConfigureAwait(false))
                        throw new ZLinkConfigurationException(
                            "The claimed MeshNode descriptor could not enter Serving state.");
                }
            }
            catch (Exception startFailure)
            {
                try
                {
                    await DisposeGenerationAsync().ConfigureAwait(false);
                }
                catch (Exception cleanupFailure)
                {
                    throw new AggregateException(startFailure, cleanupFailure);
                }

                throw;
            }
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask StopAsync(CancellationToken cancellationToken = default)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await DisposeGenerationAsync().ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask<bool> MarkDrainingAsync(
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var published = true;
            if (_clientServerDiscovery is not null)
                published &= await _clientServerDiscovery
                    .MarkDrainingAsync(cancellationToken)
                    .ConfigureAwait(false);
            if (_fanoutDiscovery is not null)
                published &= await _fanoutDiscovery
                    .MarkDrainingAsync(cancellationToken)
                    .ConfigureAwait(false);
            foreach (var reconciler in _reconcilers)
                published &= await reconciler.MarkDrainingAsync(cancellationToken).ConfigureAwait(false);
            return published;
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask<bool> MarkRetiringAsync(
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            try
            {
                var published = true;
                if (_clientServerDiscovery is not null)
                    published &= await _clientServerDiscovery.MarkRetiringAsync(cancellationToken)
                        .ConfigureAwait(false);
                if (_fanoutDiscovery is not null)
                    published &= await _fanoutDiscovery.MarkRetiringAsync(cancellationToken)
                        .ConfigureAwait(false);
                foreach (var reconciler in _reconcilers)
                    published &= await reconciler.MarkRetiringAsync(cancellationToken)
                        .ConfigureAwait(false);
                if (published) return true;
            }
            catch (Exception publicationFailure)
            {
                var rollbackFailures = await RestoreServingAsync().ConfigureAwait(false);
                if (rollbackFailures.Count != 0)
                {
                    rollbackFailures.Insert(0, publicationFailure);
                    throw new ZLinkRetiringPublicationRollbackException(rollbackFailures);
                }
                throw;
            }

            var failures = await RestoreServingAsync().ConfigureAwait(false);
            if (failures.Count != 0)
                throw new ZLinkRetiringPublicationRollbackException(failures);
            return false;

            async ValueTask<List<Exception>> RestoreServingAsync()
            {
                var failures = new List<Exception>();
                if (_clientServerDiscovery is not null)
                    await RestoreAsync(_clientServerDiscovery.MarkServingAsync)
                        .ConfigureAwait(false);
                if (_fanoutDiscovery is not null)
                    await RestoreAsync(_fanoutDiscovery.MarkServingAsync)
                        .ConfigureAwait(false);
                foreach (var reconciler in _reconcilers)
                    await RestoreAsync(reconciler.MarkServingAsync)
                        .ConfigureAwait(false);
                return failures;

                async ValueTask RestoreAsync(
                    Func<CancellationToken, ValueTask<bool>> restore)
                {
                    try
                    {
                        if (!await restore(CancellationToken.None).ConfigureAwait(false))
                            failures.Add(new InvalidOperationException(
                                "A descriptor did not confirm its Serving rollback."));
                    }
                    catch (Exception error)
                    {
                        failures.Add(error);
                    }
                }
            }
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask<bool> MarkServingAsync(
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var restored = true;
            if (_clientServerDiscovery is not null)
                restored &= await _clientServerDiscovery.MarkServingAsync(cancellationToken)
                    .ConfigureAwait(false);
            if (_fanoutDiscovery is not null)
                restored &= await _fanoutDiscovery.MarkServingAsync(cancellationToken)
                    .ConfigureAwait(false);
            foreach (var reconciler in _reconcilers)
                restored &= await reconciler.MarkServingAsync(cancellationToken)
                    .ConfigureAwait(false);
            return restored;
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask FreezeOwnerWritesAsync(
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            foreach (var reconciler in _reconcilers)
                await reconciler.FreezeOwnerWritesAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    public ValueTask DisposeAsync()
    {
        Task task;
        TaskCompletionSource? start = null;
        lock (_disposeGate)
        {
            if (_disposeTask is null)
            {
                Volatile.Write(ref _disposed, 1);
                start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = DisposeCoreAsync(start.Task);
            }
            task = _disposeTask;
        }
        start?.TrySetResult();
        return new ValueTask(task);
    }

    private async Task DisposeCoreAsync(Task started)
    {
        await started.ConfigureAwait(false);
        await _lifecycleGate.WaitAsync(CancellationToken.None).ConfigureAwait(false);
        try
        {
            await DisposeGenerationAsync().ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
            _lifecycleGate.Dispose();
        }
    }

    private async ValueTask DisposeGenerationAsync()
    {
        var clientServerDiscovery = _clientServerDiscovery;
        _clientServerDiscovery = null;
        var fanoutDiscovery = _fanoutDiscovery;
        _fanoutDiscovery = null;
        var loops = _loops.ToArray();
        var reconcilers = _reconcilers.ToArray();
        _loops.Clear();
        _reconcilers.Clear();
        _routeMeshReconcilers.Clear();
        _localLoops.Clear();
        List<Exception>? failures = null;
        if (clientServerDiscovery is not null)
        {
            try
            {
                await clientServerDiscovery.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }
        if (fanoutDiscovery is not null)
        {
            try
            {
                await fanoutDiscovery.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }
        foreach (var loop in loops)
        {
            try
            {
                await loop.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }

        if (failures is { Count: 1 })
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
    }

    private void AddLoop(
        ZLinkLocationAutoConnectType type,
        string meshName,
        ZLinkLocationRole role,
        RoutingId? nodeRid,
        string endpoint,
        uint weight,
        IZLinkAutoConnectExecutor executor,
        bool retainRemovedMembers = false,
        ulong lifecycleGeneration = 0,
        ZLinkMeshNodeObjectRole objectRole = ZLinkMeshNodeObjectRole.None,
        IReadOnlyList<ZLinkObjectCapability>? objectCapabilities = null,
        long applicationVersion = 0,
        string? maintenanceWave = null,
        string? entrySpotId = null,
        int placementWeight = 100,
        ZLinkPlacementCapacity? capacity = null,
        ZLinkActivationConcurrency? activationConcurrency = null,
        IReadOnlyDictionary<string, int>? channelWeights = null,
        ZLinkMeshNodeStartupState? startupState = null)
    {
        // MeshNode descriptors always require a physical node identity.
        var advertisable = nodeRid is { Size: > 0 };
        if (!advertisable) return;

        var local = new ZLinkAutoConnectLocal(
            type,
            meshName,
            role,
            nodeRid,
            endpoint,
            objectRole,
            channelWeights?.Count > 0);
        var effectiveLifecycleGeneration = lifecycleGeneration == 0
            ? CreateLifecycleNonce()
            : lifecycleGeneration;
        var row = startupState?.Descriptor ?? (advertisable
            ? new ZLinkMeshNodeDescriptor(
                meshName, nodeRid!.Value,
                effectiveLifecycleGeneration, DescriptorRevision: 1,
                endpoint,
                channelWeights is null
                    ? new Dictionary<string, int>(StringComparer.Ordinal)
                    {
                        [meshName] = (int)weight
                    }
                    : new Dictionary<string, int>(
                        channelWeights,
                        StringComparer.Ordinal),
                SecurityIdentity: ZLinkTransportSecurityIdentity.Plaintext,
                OwnerId: string.Empty,
                LeaseGeneration: 0,
                UpdatedAt: default)
            {
                ApplicationVersion = applicationVersion,
                ObjectRole = objectRole,
                ObjectCapabilities =
                    objectCapabilities ?? Array.Empty<ZLinkObjectCapability>(),
                MaintenanceWave = maintenanceWave,
                EntrySpotId = entrySpotId,
                State = ZLinkFrameworkRuntimeState.Serving,
                PlacementWeight = placementWeight,
                Capacity = capacity ?? new ZLinkPlacementCapacity(
                    new ZLinkPopulationCapacity(0, 0, 0),
                    new ZLinkPopulationCapacity(0, 0, 0),
                    Array.Empty<ZLinkSpotTypeCapacity>()),
                ActivationConcurrency = activationConcurrency
                    ?? new ZLinkActivationConcurrency(0, 128)
            }
            : null);
        var reconciler = new ZLinkAutoConnectReconciler(
            local, row, _runtime, _peers, executor, _options, _time,
            retainRemovedMembers,
            initiallyPublished: startupState is not null,
            initialStoreGeneration: startupState?.StoreGeneration ?? 0);
        _reconcilers.Add(reconciler);
        _localReconcilers[(type, meshName, role)] = reconciler;
        if (type is ZLinkLocationAutoConnectType.RouteMesh
            or ZLinkLocationAutoConnectType.SpotMesh)
            _routeMeshReconcilers[meshName] = reconciler;
        var loop = new ZLinkAutoConnectLoop(
            reconciler, local, _options, _store, _watchStore, _time, _leaseTracker);
        _loops.Add(loop);
        _localLoops[(type, meshName, role)] = loop;
    }

    public ZLinkRouteMeshTargetClassification ClassifyRouteMeshTarget(
        string meshName,
        RoutingId nodeRid)
    {
        return _routeMeshReconcilers.TryGetValue(meshName, out var reconciler)
            ? reconciler.ClassifyTarget(nodeRid)
            : ZLinkRouteMeshTargetClassification.Unknown;
    }

    public IReadOnlyList<ZLinkRouteMeshPeerIdentity>? GetCompleteRouteMeshPeers(
        string meshName)
    {
        return _routeMeshReconcilers.TryGetValue(meshName, out var reconciler)
            ? reconciler.CompleteMeshPeers()
            : null;
    }

    internal void SetLocalWeight(
        ZLinkLocationAutoConnectType type,
        string meshName,
        ZLinkLocationRole role,
        uint weight)
    {
        if (_localReconcilers.TryGetValue((type, meshName, role), out var reconciler))
            reconciler.SetLocalWeight(weight);
    }

    internal void SetLocalPlacementWeight(string meshName, int weight)
    {
        if (_localReconcilers.TryGetValue(
                (ZLinkLocationAutoConnectType.SpotMesh, meshName, ZLinkLocationRole.Spot),
                out var reconciler))
            reconciler.SetLocalPlacementWeight(weight);
    }

    internal void SetLocalActivationConcurrency(string meshName, int active)
    {
        if (!_localReconcilers.TryGetValue(
                (ZLinkLocationAutoConnectType.SpotMesh, meshName, ZLinkLocationRole.Spot),
                out var reconciler))
            return;

        reconciler.SetLocalActivationConcurrency(active);
        if (_localLoops.TryGetValue(
                (ZLinkLocationAutoConnectType.SpotMesh, meshName, ZLinkLocationRole.Spot),
                out var loop))
            loop.Wake();
    }

    internal void SetLocalChannelWeight(
        string meshName,
        string channelName,
        int weight)
    {
        if (_localReconcilers.TryGetValue(
                (ZLinkLocationAutoConnectType.SpotMesh, meshName, ZLinkLocationRole.Spot),
                out var reconciler))
            reconciler.SetLocalChannelWeight(channelName, weight);
    }

    internal void SetClientServerWeight(string channelName, int weight)
    {
        _clientServerDiscovery?.SetLocalWeight(channelName, weight);
    }

    internal ValueTask<bool> SetLocalWeightAsync(
        ZLinkLocationAutoConnectType type,
        string meshName,
        ZLinkLocationRole role,
        uint weight,
        CancellationToken cancellationToken)
    {
        if (!_localReconcilers.TryGetValue((type, meshName, role), out var reconciler))
            return ValueTask.FromResult(true);
        return type == ZLinkLocationAutoConnectType.SpotMesh
            ? reconciler.SetAllLocalChannelWeightsAsync(weight, cancellationToken)
            : reconciler.SetLocalWeightAsync(weight, cancellationToken);
    }

    private static RoutingId? RidOrNull(RoutingId routingId) =>
        routingId.Size > 0 ? routingId : null;

    private static ulong CreateLifecycleNonce()
    {
        Span<byte> bytes = stackalloc byte[sizeof(ulong)];
        ulong value;
        do
        {
            System.Security.Cryptography.RandomNumberGenerator.Fill(bytes);
            value = System.Buffers.Binary.BinaryPrimitives
                .ReadUInt64BigEndian(bytes);
        } while (value == 0);
        return value;
    }

    private static IReadOnlyList<ZLinkObjectCapability> BuildObjectCapabilities(
        ZLinkSpotNodeRegistration registration)
    {
        var capabilities = new List<ZLinkObjectCapability>(
            registration.SpotRelocations.Count
            + registration.InstanceSpotRelocations.Count
            + registration.ActorRelocations.Count);
        AddCapabilities(
            capabilities,
            ZLinkPlacementObjectKind.UserSpot,
            registration.SpotRelocations);
        AddCapabilities(
            capabilities,
            ZLinkPlacementObjectKind.InstanceSpot,
            registration.InstanceSpotRelocations);
        AddCapabilities(
            capabilities,
            ZLinkPlacementObjectKind.Actor,
            registration.ActorRelocations);
        return capabilities
            .OrderBy(static capability => capability.ObjectKind)
            .ThenBy(static capability => capability.StableType, StringComparer.Ordinal)
            .ToArray();
    }

    private static void AddCapabilities(
        ICollection<ZLinkObjectCapability> capabilities,
        ZLinkPlacementObjectKind objectKind,
        IReadOnlyDictionary<string, ZLinkObjectRelocationRegistration>
            registrations)
    {
        foreach (var (stableType, registration) in registrations)
        {
            capabilities.Add(new ZLinkObjectCapability(
                objectKind,
                stableType,
                registration.PolicyKind switch
                {
                    0 => ZLinkObjectMaintenancePolicyKind.Disabled,
                    1 => ZLinkObjectMaintenancePolicyKind.Recreate,
                    2 => ZLinkObjectMaintenancePolicyKind.Snapshot,
                    _ => throw new ZLinkConfigurationException(
                        $"Unknown relocation policy kind '{registration.PolicyKind}'.")
                },
                registration.AdapterType is not null,
                objectKind == ZLinkPlacementObjectKind.Actor
                    ? 0
                    : registration.Placement.MaxActiveObjects ?? 0));
        }
    }

    private static IReadOnlyList<ZLinkSpotTypeCapacity>
        BuildSpotTypeCapacities(ZLinkSpotNodeRegistration registration) =>
        BuildObjectCapabilities(registration)
            .Where(static capability =>
                capability.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot)
            .Select(static capability => new ZLinkSpotTypeCapacity(
                capability.ObjectKind,
                capability.StableType,
                0,
                0,
                capability.Limit))
            .ToArray();

    private sealed class SpotRouterExecutor(
        ZLinkSpotNodeRuntime node,
        bool connectRouter) : IZLinkAutoConnectExecutor
    {
        public bool Connect(ZLinkAutoConnectTarget target)
        {
            node.ObserveRequestSourceFence(target);
            node.ObservePeerExpectation(target);
            if (!connectRouter || !target.InitiatesConnection) return true;
            return node.ConnectPeerAuto(
                target.NodeRid,
                target.Endpoint,
                ZLinkTransportSecurityIdentity.ToAdmissionIdentity(
                    target.SecurityIdentity));
        }

        public bool Disconnect(ZLinkAutoConnectTarget target)
        {
            // Row absence alone must not tear down a live admitted transport
            // (store outages and lease expiry windows ride established
            // connections, SF-B2). The non-initiating side has no auto-connect
            // intent of its own, so it removes only an admission-pending peer.
            node.ForgetPeerExpectation(target);
            if (!target.InitiatesConnection)
                return node.DisconnectPeerBeforeAdmission(target);
            if (!connectRouter) return true;
            return node.DisconnectPeerAuto(target.NodeRid, target.Endpoint);
        }
    }

}
