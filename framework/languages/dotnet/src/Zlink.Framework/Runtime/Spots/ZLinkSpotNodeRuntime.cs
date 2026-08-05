using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotNodeRuntime : IAsyncDisposable
{
    private readonly ZLinkSpotNodeBundleRegistry _bundles;
    private readonly ZLinkActivationConcurrencyAdmission _activationAdmission;
    private readonly ZLinkFrameworkRegistration _frameworkRegistration;
    private readonly ZLinkSpotMonitoringSnapshotProvider _monitoringSnapshots;
    private readonly ZLinkSpotPeerConnectionSet _peerConnections = new();
    private readonly ZLinkSpotPeerConnector _peerConnector;
    private readonly ZLinkAsyncSubmitter _nodeSubmitter;
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly IServiceProvider _services;
    private readonly ZLinkSpotNodeCatalog _spots;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly ZLinkCompletionAdmissionOwner _completionAdmission;
    private readonly ZLinkTimerScheduler _timerScheduler;
    private readonly ZLinkLocationLifecycle? _locationLifecycle;
    private readonly object _disposeGate = new();
    private Task? _disposeTask;
    private IDisposable? _manualConnectionAttachment;
    private bool _stopSourceDisposed;
    private IActorCreateOperationTarget? _actorCreateOperationTarget;
    private IActorDestroyOperationTarget? _actorDestroyOperationTarget;
    private ZLinkInstanceSpotActivationTarget? _instanceSpotActivationTarget;
    private IUserSpotOperationTarget? _userSpotOperationTarget;
    private IRelocationReplyRelayTarget? _relocationReplyRelayTarget;
    private ZLinkCanonicalRelocationReservationOwner?
        _canonicalRelocationReservationOwner;
    private readonly ZLinkServiceWireCodec.RequestSourceFence?
        _localRequestSource;
    private IZLinkBackendSpot? _entrySpot;
    private ZLinkEntrySpotDispatchPump? _entryDispatchPump;
    private ZLinkSpotOutboundTransport? _entryOutbound;
    private ZLinkEntrySpotActivation? _entrySpotActivation;
    private ZLinkMeshNodeRouteDispatcher? _nodeRouteDispatcher;
    private int _entrySpotMetricActive;
    private int _entrySpotLifecycleClosed;

    public ZLinkSpotNodeRuntime(
        IServiceProvider services,
        ZLinkFrameworkRuntime runtime,
        ZLinkFrameworkRegistration frameworkRegistration,
        ZLinkSpotNodeRegistration registration,
        IZLinkBackendContext context,
        IZLinkChannelBackendAdapter channelAdapter,
        IZLinkBackendSpotNode node,
        ZLinkCompletionAdmissionOwner completionAdmission,
        ZLinkTimerScheduler timerScheduler,
        string spotChannelName,
        ZLinkLocationLifecycle? locationLifecycle,
        ZLinkMeshNodeStartupState? startupState = null,
        string? entrySpotId = null)
    {
        _services = services;
        _runtime = runtime;
        _frameworkRegistration = frameworkRegistration;
        Registration = registration;
        Node = node;
        _completionAdmission = completionAdmission;
        _timerScheduler = timerScheduler;
        _locationLifecycle = locationLifecycle;
        _activationAdmission = new(
            registration.MaxPendingActivations,
            active => runtime.SetActivationConcurrency(spotChannelName, active));
        if (node is IZLinkBackendActorMessageFollowIngress
            messageFollowIngress)
        {
            messageFollowIngress.SetActorMessageFollowIngressAdmission(
                ingress => ZLinkActorHandoffIngress
                    .CanAdmitStaleManagedIngress(runtime, ingress));
            messageFollowIngress.SetActorMessageFollowIngressHandler(
                parts => ZLinkActorHandoffIngress
                    .TryCaptureOrFollowStaleManagedIngress(runtime, parts));
        }
        if (node is IZLinkBackendMessageFollowNotifications
            messageFollowNotifications)
        {
            messageFollowNotifications.SetMessageFollowNotificationHandler(
                (sourceNodeRid, record) =>
                {
                    if (sourceNodeRid != record.Source.TargetNodeRid)
                        return;
                    services.GetService<ZLinkStoreLocationResolvers>()
                        ?.InvalidateMessageFollowRoute(record);
                });
        }
        if (locationLifecycle is not null)
        {
            if (node is not IZLinkBackendAuthorityObserver authorityObserver)
                throw new InvalidOperationException(
                    "The MeshNode backend does not support authority fencing.");
            authorityObserver.SetLocalOwnerLeaseGeneration(
                checked((ulong)locationLifecycle.OwnerToken.LeaseGeneration));
            if (node is not IZLinkBackendRequestSourceFenceObserver sourceObserver)
                throw new InvalidOperationException(
                    "The MeshNode backend does not preserve request-source fences.");
            var localRequestSource =
                new ZLinkServiceWireCodec.RequestSourceFence(
                    locationLifecycle.OwnerToken.OwnerId,
                    checked((ulong)locationLifecycle.OwnerToken.LeaseGeneration),
                    node.RoutingId,
                    node.MeshStatus().LifecycleGeneration);
            sourceObserver.SetLocalRequestSourceFence(localRequestSource);
            sourceObserver.ObserveRequestSourceFence(localRequestSource);
            _localRequestSource = localRequestSource;
        }
        StartupState = startupState;
        EntrySpotId = entrySpotId ?? startupState?.EntrySpotId
            ?? registration.EntrySpotId;
        _taskRunner = new ZLinkRuntimeTaskRunner(
            runtime.ErrorSink,
            _stopSource.Token,
            runtime.ExecutionOwner);
        _monitoringSnapshots = new ZLinkSpotMonitoringSnapshotProvider(node);
        _peerConnector = new ZLinkSpotPeerConnector(node, _peerConnections);
        _nodeSubmitter = new ZLinkAsyncSubmitter(
            node.OnSendReady,
            frameworkRegistration.DefaultSocketSendTimeout,
            _stopSource.Token,
            ZLinkAsyncSubmitter.ResolvePendingCapacity(),
            completionAdmission: _completionAdmission);
        _bundles = new ZLinkSpotNodeBundleRegistry(
            frameworkRegistration,
            node,
            _stopSource.Token);
        _spots = new ZLinkSpotNodeCatalog(
            services,
            runtime,
            frameworkRegistration,
            registration,
            node,
            spotChannelName,
            _completionAdmission,
            locationLifecycle,
            _timerScheduler,
            _activationAdmission);
        _spots.StartIdleEviction();
        if (frameworkRegistration.Locations.ResolveStore() is not null
            && node is IZLinkBackendRelocationReplyRelay relayBackend)
        {
            _relocationReplyRelayTarget =
                new ZLinkRelocationReplyTarget(
                    relayBackend);
            relayBackend.SetRelocationReplyRelayTarget(
                _relocationReplyRelayTarget);
        }
        if (frameworkRegistration.Locations.ResolveStore()
                is { } locationStore
            && node is IZLinkBackendCanonicalRelocationReservation
                canonicalBackend
            && startupState is { } canonicalStartup
            && (registration.SpotRelocations.Values.Any(
                    static relocation => relocation.PolicyKind != 0)
                || registration.InstanceSpotRelocations.Values.Any(
                    static relocation => relocation.PolicyKind != 0)
                || registration.ActorRelocations.Values.Any(
                    static relocation => relocation.PolicyKind != 0)))
        {
            _canonicalRelocationReservationOwner =
                new ZLinkCanonicalRelocationReservationOwner(
                    locationStore,
                    runtime.RelocationPermits,
                    spotChannelName,
                    node.RoutingId,
                    canonicalStartup.Descriptor.LifecycleGeneration,
                    frameworkRegistration.DefaultRequestTimeout,
                    relocationStore:
                        frameworkRegistration.Locations.ResolveRelocationStore()
                        ?? throw new ZLinkConfigurationException(
                            "Relocation Store is not registered."),
                    targetRuntime:
                        services.GetRequiredService<
                            ZLinkSpotRetireTargetRuntime>(),
                    standaloneActorRuntime:
                        runtime.StandaloneActorRelocationRuntime,
                    targetReady: () => services
                        .GetRequiredService<IZLinkRouteMeshRuntime>()
                        .GetStatus(spotChannelName)
                        .IsReady);
            canonicalBackend.SetCanonicalRelocationReservationTarget(
                _canonicalRelocationReservationOwner);
        }
        if (registration.SpotRelocations.Count > 0
            && frameworkRegistration.Locations.ResolveStore() is { } authorityStore)
        {
            _userSpotOperationTarget = new ZLinkUserSpotOperationTarget(
                authorityStore,
                _spots,
                node,
                registration,
                frameworkRegistration.Codecs);
            node.SetUserSpotOperationTarget(_userSpotOperationTarget);
        }
        if (registration.ActorFactories.Count > 0
            && frameworkRegistration.Locations.ResolveStore() is { } actorAuthorityStore)
        {
            var actorOperationTarget = new ZLinkActorOperationTarget(
                actorAuthorityStore,
                runtime,
                node,
                spotChannelName,
                frameworkRegistration.Codecs);
            _actorCreateOperationTarget = actorOperationTarget;
            _actorDestroyOperationTarget = actorOperationTarget;
            node.SetActorCreateOperationTarget(_actorCreateOperationTarget);
            node.SetActorDestroyOperationTarget(_actorDestroyOperationTarget);
        }
        if (registration.InstanceSpotFactories.Count > 0
            && frameworkRegistration.Locations.ResolveStore()
                is { } instanceLocationStore
            && frameworkRegistration.Locations.ResolveRelocationStore()
                is { } relocationStore
            && locationLifecycle is not null)
        {
            _instanceSpotActivationTarget = new ZLinkInstanceSpotActivationTarget(
                instanceLocationStore,
                relocationStore,
                _spots,
                node,
                registration,
                locationLifecycle.OwnerToken);
            node.SetInstanceSpotActivationTarget(_instanceSpotActivationTarget);
        }
    }

    internal ZLinkServiceWireCodec.RequestSourceFence LocalRequestSource =>
        _localRequestSource
        ?? throw new InvalidOperationException(
            "The MeshNode does not have a Location Store owner fence.");

    internal bool TryTakeCanonicalRelocationPermit(
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration,
        long actualPayloadBytes,
        out IDisposable permit,
        out ZLinkPreparedAggregateRelocation? preparedAggregate,
        out ulong targetAuthorityOwnerGeneration)
    {
        if (_canonicalRelocationReservationOwner is not null)
            return _canonicalRelocationReservationOwner.TryTakeStagingPermit(
                relocationId, targetAttemptGeneration, actualPayloadBytes,
                out permit, out preparedAggregate,
                out targetAuthorityOwnerGeneration);
        permit = null!;
        preparedAggregate = null!;
        targetAuthorityOwnerGeneration = 0;
        return false;
    }

    internal void BeginCanonicalRelocationStaging(
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration)
    {
        (_canonicalRelocationReservationOwner
            ?? throw new InvalidOperationException(
                "Canonical relocation reservation owner is not configured."))
            .BeginStaging(relocationId, targetAttemptGeneration);
    }

    internal ZLinkMeshNodeStartupState? StartupState { get; }

    internal string EntrySpotId { get; }

    public string Name => Registration.SpotNodeName;

    internal ZLinkSpotNodeCatalog Catalog => _spots;

    public IReadOnlySet<Type> SpotFactories => Registration.SpotFactories;

    public IZLinkBackendSpotNode Node { get; }

    internal ZLinkSpotNodeRegistration Registration { get; }

    internal ZLinkActivationConcurrencyAdmission ActivationAdmission =>
        _activationAdmission;

    internal async ValueTask<(
        UserSpotCreateCompletion Completion,
        IReadOnlyList<Message> Reply)> CreateUserSpotLocalAsync(
        string spotId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        CancellationToken cancellationToken)
    {
        var target = _userSpotOperationTarget
                     ?? throw new ZLinkFrameworkException(
                         ZLinkFrameworkErrorKind.InvalidOperation,
                         $"MeshNode '{Name}' does not host User Spot factories.");
        var remaining = checked((long)deadlineUnixMs)
                        - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (remaining <= 0)
            throw new TimeoutException("The User Spot create deadline elapsed.");

        var operationId = Node.AllocateOperationId();
        var correlation = operationId.Low;
        var status = Node.MeshStatus();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stopSource.Token);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(remaining));
        var terminal = await target.CreateAsync(
                new UserSpotCreateOperation(
                    correlation,
                    operationId,
                    Node.RoutingId,
                    status.LifecycleGeneration,
                    spotId,
                    stableType,
                    reservation,
                    deadlineUnixMs),
                deadline.Token)
            .ConfigureAwait(false);
        if (terminal.Result != RequestResult.Ok
            || terminal.Completion is not UserSpotCreateCompletion completion)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                "Local User Spot create failed.");

        var reply = terminal.ReplyParts is null
            ? Array.Empty<Message>()
            : terminal.ReplyParts.Select(static part => Message.From(part.Span)).ToArray();
        return (completion, reply);
    }

    internal ValueTask<InstanceSpotActivationTerminal> ActivateInstanceSpotLocalAsync(
        InstanceSpotActivationTarget target,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        MeshOperationId operationId,
        string sourceSpotId,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        bool request,
        ulong deadlineUnixMs,
        ReadOnlyMemory<byte>? metadata,
        CancellationToken cancellationToken)
    {
        var activationTarget = _instanceSpotActivationTarget
                               ?? throw new ZLinkFrameworkException(
                                   ZLinkFrameworkErrorKind.InvalidOperation,
                                   $"MeshNode '{Name}' does not host Instance Spot factories.");
        return activationTarget.ActivateAsync(
            new InstanceSpotActivationOperation(
                target,
                sourceNodeRid,
                sourceNodeGeneration,
                sourceSpotId,
                operationId,
                request,
                request ? operationId.Low : 0,
                deadlineUnixMs),
            metadata,
            payload,
            cancellationToken);
    }

    internal async ValueTask<(
        ActorCreateCompletion Completion,
        IReadOnlyList<Message> Reply)> CreateActorLocalAsync(
        string actorId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        CancellationToken cancellationToken)
    {
        var target = _actorCreateOperationTarget
                     ?? throw new ZLinkFrameworkException(
                         ZLinkFrameworkErrorKind.InvalidOperation,
                         $"MeshNode '{Name}' does not host Actor factories.");
        var remaining = checked((long)deadlineUnixMs)
                        - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (remaining <= 0)
            throw new TimeoutException("The Actor create deadline elapsed.");

        var operationId = Node.AllocateOperationId();
        var correlation = operationId.Low;
        var status = Node.MeshStatus();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stopSource.Token);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(remaining));
        var terminal = await target.CreateAsync(
                new ActorCreateOperation(
                    correlation,
                    operationId,
                    Node.RoutingId,
                    status.LifecycleGeneration,
                    actorId,
                    stableType,
                    reservation,
                    deadlineUnixMs),
                deadline.Token)
            .ConfigureAwait(false);
        if (terminal.Result != RequestResult.Ok || terminal.Completion is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"Local Actor create failed with '{terminal.Result}'/"
                + $"'{terminal.FailureCode}'.");

        var reply = terminal.ReplyParts is null
            ? Array.Empty<Message>()
            : terminal.ReplyParts.Select(static part => Message.From(part.Span)).ToArray();
        return (terminal.Completion, reply);
    }

    internal ValueTask<ActorDestroyOperationTerminal> DestroyActorLocalAsync(
        ActorDestroyOperation operation,
        CancellationToken cancellationToken)
    {
        var target = _actorDestroyOperationTarget
                     ?? throw new ZLinkFrameworkException(
                         ZLinkFrameworkErrorKind.InvalidOperation,
                         $"MeshNode '{Name}' does not host Actor factories.");
        return target.DestroyAsync(operation, cancellationToken);
    }

    internal bool UsesManualRouterAcquisition =>
        Registration.Router?.AcquisitionMode == ZLinkPeerAcquisitionMode.Manual;

    internal bool IsExplicitManualRouterRouteDisconnected(RoutingId targetNodeRid)
    {
        if (Registration.Router is not
            {
                AcquisitionMode: ZLinkPeerAcquisitionMode.Manual
            } router)
            return false;

        var targetEndpoints = router.PeerRoutingIds
            .Where(pair => pair.Value == targetNodeRid)
            .Select(pair => pair.Key)
            .ToArray();
        if (targetEndpoints.Length == 0) return false;

        var configuredEndpoints = router.ManualConnections.ListConnections();
        return targetEndpoints.All(endpoint => !configuredEndpoints.Contains(endpoint, StringComparer.Ordinal));
    }

    internal ZLinkRouteMeshTargetClassification ClassifyManualRouterTarget(
        RoutingId targetNodeRid)
    {
        if (Registration.Router is not
            {
                AcquisitionMode: ZLinkPeerAcquisitionMode.Manual
            } router)
            return ZLinkRouteMeshTargetClassification.Unknown;

        var peer = Node.MeshPeers().FirstOrDefault(candidate =>
            candidate.RoutingId == targetNodeRid);
        if (peer is not null)
        {
            if (peer.ObjectRole == ZLinkMeshNodeObjectRole.Client)
                return ZLinkRouteMeshTargetClassification.ObjectClientTarget;
            if (peer.State == MeshPeerState.Admitted)
                return ZLinkRouteMeshTargetClassification.ReadyEligible;
        }

        var matchingEndpoints = router.PeerRoutingIds
            .Where(pair => pair.Value == targetNodeRid)
            .Select(pair => pair.Key)
            .ToArray();
        if (matchingEndpoints.Length == 0)
        {
            return _peerConnections.HasRetainedManualPeer(targetNodeRid)
                ? ZLinkRouteMeshTargetClassification.RequiredNotConnected
                : ZLinkRouteMeshTargetClassification.Unknown;
        }

        return router.ManualConnections.ListConnections().Any(endpoint =>
            matchingEndpoints.Contains(endpoint, StringComparer.Ordinal))
                ? ZLinkRouteMeshTargetClassification.RequiredNotConnected
                : ZLinkRouteMeshTargetClassification.Unknown;
    }

    public IReadOnlyCollection<ZLinkSpotActivation> Spots => _spots.Spots;

    internal ZLinkEntrySpotActivation? EntrySpotActivation => _entrySpotActivation;

    internal ZLinkSpotOutboundTransport EntryOutbound => _entryOutbound
        ?? throw new InvalidOperationException($"SPOT node '{Name}' entry outbound transport is not initialized.");

    internal bool TrySendToNodeOnce(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata = default)
    {
        return ZLinkSubmitFailureMapper.AcceptOrThrow(
            Node.SendToNode(targetNodeRid, parts, SendFlags.DontWait, metadata),
            $"node '{targetNodeRid}'");
    }

    internal ValueTask<ZLinkOneWaySubmitResult> SendToNodeAsync(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        if (targetNodeRid == Node.RoutingId)
            return SubmitToLocalNodeAsync(parts, cancellationToken, metadata);

        return _nodeSubmitter.SubmitAsync(
            parts,
            pending => ZLinkSubmitFailureMapper.AcceptOrThrow(
                Node.SendToNode(targetNodeRid, pending, SendFlags.DontWait, metadata),
                $"node '{targetNodeRid}'"),
            cancellationToken);
    }

    private ValueTask<ZLinkOneWaySubmitResult> SubmitToLocalNodeAsync(
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata)
    {
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_stopSource.IsCancellationRequested)
                return ValueTask.FromResult(new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Shutdown));
            if (_nodeRouteDispatcher is null)
                return ValueTask.FromResult(new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.TargetNotFound));
            if (!ZLinkMeshMetadataCodec.TryDecode(metadata.Span, out var decodedMetadata))
                throw new ArgumentException("Application metadata is malformed.", nameof(metadata));

            var received = new ZLinkBackendRouteReceived(
                parts,
                Node.RoutingId,
                spotId: null,
                requestSeq: null,
                reply: null,
                metadata: decodedMetadata);
            if (_nodeRouteDispatcher.TryDispatch(received))
                return ValueTask.FromResult(new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Submitted));

            received.Dispose();
            return ValueTask.FromResult(new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Shutdown));
        }
        catch
        {
            ZLinkMessageParts.DisposeAll(parts);
            throw;
        }
    }

    internal ValueTask<ZLinkOneWaySubmitResult> SendToActorAsync(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        return _nodeSubmitter.SubmitAsync(
            parts,
            pending => ZLinkSubmitFailureMapper.AcceptOrThrow(
                Node.SendToActor(actor, pending, SendFlags.DontWait),
                $"actor '{actor.ActorId}'"),
            cancellationToken);
    }

    internal void ObserveActorAuthority(
        ZLinkBackendActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        if (Node is not IZLinkBackendAuthorityObserver observer)
            throw new InvalidOperationException(
                "The MeshNode backend does not support authority fencing.");
        observer.ObserveActorAuthority(
            actor,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
    }

    internal ValueTask<IReadOnlyList<Message>> RequestToNodeAsync(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        return _nodeSubmitter.SubmitRequestAsync<IReadOnlyList<Message>>(
            parts,
            (pending, complete, fail) => Node.RequestToNode(
                targetNodeRid,
                pending,
                (result, reply) =>
                {
                    //  The mesh layer's own view of the round trip. If this
                    //  never fires the reply did not reach this node at all.
                    Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"node_request_result target={targetNodeRid} result={result}");
                    if (result == RequestResult.Ok)
                    {
                        complete(reply);
                        return;
                    }

                    fail(ZLinkRequestFailureMapper.CreateCompletionException(
                        result,
                        $"Node request to '{targetNodeRid}' failed with result '{result}'."));
                    ZLinkMessageParts.DisposeAll(reply);
                },
                SendFlags.DontWait,
                timeout,
                metadata),
            cancellationToken,
            ZLinkMessageParts.DisposeAll);
    }

    internal ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord>
        RelayRelocationReplyAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.ReplyRelayRecord relay,
            ZLinkServiceWireCodec.RequestSourceFence expectedSource,
            IReadOnlyList<Message> payload,
            TimeSpan timeout,
            CancellationToken cancellationToken) =>
        Node is IZLinkBackendRelocationReplyRelay backend
            ? backend.RelayRelocationReplyAsync(
                targetNodeRid,
                relay,
                expectedSource,
                payload,
                timeout,
                cancellationToken)
            : ValueTask.FromException<
                ZLinkServiceWireCodec.ReplyRelayAckRecord>(
                new NotSupportedException(
                    "The MeshNode backend does not support relocation reply relay."));

    internal void RequestStop()
    {
        lock (_disposeGate)
        {
            if (_stopSourceDisposed) return;
            _stopSource.Cancel();
        }
        _spots.RequestStop();
        _entryDispatchPump?.RequestStop();
        _entrySpotActivation?.RequestStop();
    }

    internal void CancelActiveOperations()
    {
        _spots.CancelActiveOperations();
    }

    internal async ValueTask CloseLifecycleAsync()
    {
        await _spots.CloseLifecycleAsync().ConfigureAwait(false);
        if (_entrySpotActivation is not { } activation
            || Interlocked.Exchange(ref _entrySpotLifecycleClosed, 1) != 0)
            return;

        await activation.CloseAsync(CancellationToken.None).ConfigureAwait(false);
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
            return new ValueTask(
                _disposeTask ??= DisposeCoreAsync(CancellationToken.None));
    }

    internal ValueTask ForceStopAsync(CancellationToken cancellationToken)
    {
        lock (_disposeGate)
            return new ValueTask(
                _disposeTask ??= DisposeCoreAsync(cancellationToken));
    }

    private async Task DisposeCoreAsync(CancellationToken forceStopToken)
    {
        var failures = new List<Exception>();
        Capture(DetachManualConnections);
        if (!forceStopToken.CanBeCanceled)
            await CaptureAsync(CloseLifecycleAsync).ConfigureAwait(false);
        Capture(RequestStop);
        if (_entryDispatchPump is { } entryDispatchPump)
            await CaptureAsync(entryDispatchPump.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_taskRunner.StopAsync).ConfigureAwait(false);
        await CaptureAsync(forceStopToken.CanBeCanceled
                ? _spots.ForceStopAsync
                : _spots.DisposeAsync)
            .ConfigureAwait(false);
        await CaptureAsync(_bundles.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(
                () => DisposeEntrySpotAsync(forceStopToken.CanBeCanceled))
            .ConfigureAwait(false);
        await CaptureAsync(_nodeSubmitter.DisposeAsync).ConfigureAwait(false);
        if (_canonicalRelocationReservationOwner is { } reservationOwner)
            await CaptureAsync(reservationOwner.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(forceStopToken.CanBeCanceled
                ? () => Node.ForceStopAsync(forceStopToken)
                : Node.DisposeAsync)
            .ConfigureAwait(false);
        Capture(() =>
        {
            lock (_disposeGate)
            {
                if (_stopSourceDisposed) return;
                _stopSource.Dispose();
                _stopSourceDisposed = true;
            }
        });
        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }

        void Capture(Action cleanup)
        {
            try
            {
                cleanup();
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    internal void OwnManualConnectionAttachment(IDisposable attachment)
    {
        ArgumentNullException.ThrowIfNull(attachment);
        IDisposable? previous = null;
        var dispose = false;
        lock (_disposeGate)
        {
            if (_disposeTask is not null)
                dispose = true;
            else
            {
                previous = _manualConnectionAttachment;
                _manualConnectionAttachment = attachment;
            }
        }
        previous?.Dispose();
        if (!dispose) return;
        attachment.Dispose();
        throw new ObjectDisposedException(nameof(ZLinkSpotNodeRuntime));
    }

    private void DetachManualConnections()
    {
        IDisposable? attachment;
        lock (_disposeGate)
        {
            attachment = _manualConnectionAttachment;
            _manualConnectionAttachment = null;
        }
        attachment?.Dispose();
    }

    public void ApplyEntrySpotIdBeforeBind()
    {
        if (string.IsNullOrEmpty(EntrySpotId)) return;

        _entrySpot = Node.EntrySpot();
        _entrySpot.SetRoutingId(
            ZLinkSpotId.ToNativeRoutingId(EntrySpotId));
    }

    public async ValueTask InitializeEntrySpotAsync()
    {
        WireNodeRouteDispatch();
        _entrySpot ??= Node.EntrySpot();
        await TrackEntrySpotLocationAsync().ConfigureAwait(false);
        if (_instanceSpotActivationTarget is not null)
            await _instanceSpotActivationTarget.RecoverAsync(_stopSource.Token)
                .ConfigureAwait(false);
        _entryOutbound ??= new ZLinkSpotOutboundTransport(
            _entrySpot,
            Registration.Router?.SocketConfig.SendTimeout
            ?? _frameworkRegistration.DefaultSocketSendTimeout,
            _stopSource.Token,
            _completionAdmission);
        if (Registration.EntrySpotType is null)
        {
            if (ShouldAttachActorDispatchPump())
            {
                _entryDispatchPump = new ZLinkEntrySpotDispatchPump(_runtime, null, _taskRunner);
                _entryDispatchPump.Attach(_entrySpot);
                if (Interlocked.Exchange(ref _entrySpotMetricActive, 1) == 0)
                    ZLinkRuntimeMetrics.RecordSpotCreated(
                        Registration.SpotNodeName,
                        "entry");
            }

            return;
        }

        var entrySpot = _entrySpot;

        var activation = await CreateEntrySpotActivationAsync(entrySpot)
            .ConfigureAwait(false);
        if (activation is not null)
        {
            if (_entrySpotActivation is not null)
                throw new InvalidOperationException(
                    $"SPOT node '{Registration.SpotNodeName}' already has an Entry Spot activation.");

            _entrySpotActivation = activation;
        }

        _entryDispatchPump = new ZLinkEntrySpotDispatchPump(_runtime, activation, _taskRunner);
        _entryDispatchPump.Attach(entrySpot);
        if (Interlocked.Exchange(ref _entrySpotMetricActive, 1) == 0)
            ZLinkRuntimeMetrics.RecordSpotCreated(
                Registration.SpotNodeName,
                "entry");
    }

    public ZLinkSpotMonitoringSnapshot GetMonitoringSnapshot()
    {
        return _monitoringSnapshots.MonitorStatus();
    }

    internal IReadOnlyList<ZLinkInstanceSpotTypeSnapshot>
        GetInstanceSpotMonitoringSnapshots()
    {
        if (Registration.InstanceSpotFactories.Count == 0)
            return Array.Empty<ZLinkInstanceSpotTypeSnapshot>();

        var activationTarget = _instanceSpotActivationTarget
            ?? throw new InvalidOperationException(
                $"MeshNode '{Name}' has Instance Spot factories without an activation target.");
        return Registration.InstanceSpotFactories.Keys
            .Order(StringComparer.Ordinal)
            .Select(stableType =>
            {
                var catalog = _spots.InstanceSpotSnapshot(stableType);
                var operations = activationTarget.MonitoringSnapshot(stableType);
                return new ZLinkInstanceSpotTypeSnapshot(
                    stableType,
                    catalog.ActiveCount,
                    catalog.ActivatingCount,
                    catalog.ClosingCount,
                    operations.PendingMessageCount,
                    operations.PendingByteCount,
                    operations.LastActivationOutcome);
            })
            .ToArray();
    }

    public ZLinkSpotPublisherBundle GetOrCreatePublisherBundle(string channelName)
    {
        return _bundles.GetOrCreatePublisherBundle(channelName);
    }

    public async ValueTask<ZLinkSpotCreateResult> CreateAsync(
        Type spotType,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return await _spots.CreateAsync(spotType, request, cancellationToken);
    }

    public async ValueTask<ZLinkSpotCreateResult> GetOrCreateAsync(
        Type spotType,
        string requestedSpotId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return await _spots.GetOrCreateAsync(
            spotType,
            requestedSpotId,
            request,
            cancellationToken);
    }

    public ValueTask<ZLinkSpotInfo?> GetAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        return _spots.GetAsync(spotId, cancellationToken);
    }

    public ValueTask<IReadOnlyList<ZLinkSpotInfo>> ListAsync(CancellationToken cancellationToken)
    {
        return _spots.ListAsync(cancellationToken);
    }

    public async ValueTask<bool> CloseAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        return await _spots.CloseAsync(spotId, cancellationToken);
    }

    internal ValueTask<ZLinkSpotDrainResult> TryDrainSpotsAsync(
        bool relocate,
        bool hostShutdown,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken) =>
        TryDrainSpotsAsync(
            relocate,
            hostShutdown,
            selection,
            ZLinkSpotRelocationPhase.Aggregates,
            DateTimeOffset.UtcNow + _frameworkRegistration.DefaultRequestTimeout,
            cancellationToken);

    internal async ValueTask<ZLinkSpotDrainResult> TryDrainSpotsAsync(
        bool relocate,
        bool hostShutdown,
        ZLinkRelocationTargetSelection selection,
        ZLinkSpotRelocationPhase phase,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
    {
        if (relocate)
            return await _spots.TryRelocateForRetireAsync(
                    selection,
                    phase,
                    cancellationToken,
                    absoluteDeadline)
                .ConfigureAwait(false);
        return new ZLinkSpotDrainResult(
            await _spots.TryDrainAsync(hostShutdown, cancellationToken)
                .ConfigureAwait(false),
            0,
            null,
            ZLinkRelocationCommitKnowledge.NotCommitted,
            true);
    }

    public ValueTask<bool> ConnectPeerAsync(string endpoint, CancellationToken cancellationToken)
    {
        return _peerConnector.ConnectPeerAsync(endpoint, cancellationToken);
    }

    public ValueTask<bool> ConnectPeerAsync(
        RoutingId peerRid,
        string endpoint,
        CancellationToken cancellationToken)
    {
        _peerConnections.RetainManualPeerRid(endpoint, peerRid);
        return _peerConnector.ConnectPeerAsync(peerRid, endpoint, cancellationToken);
    }

    public void DisconnectPeer(string endpoint)
    {
        _peerConnector.Disconnect(endpoint);
    }

    public void DisconnectPeerManual(string endpoint)
        => _peerConnector.DisconnectPeerManual(endpoint);

    public void DisconnectPeerManual(string endpoint, RoutingId peerRid)
    {
        _peerConnections.RetainManualPeerRid(endpoint, peerRid);
        _peerConnector.DisconnectPeerManual(endpoint);
    }

    public bool ConnectPeerAuto(
        RoutingId? peerRid,
        string endpoint,
        string expectedSecurityIdentity)
        => _peerConnector.ConnectPeerAuto(
            peerRid,
            endpoint,
            expectedSecurityIdentity);

    internal void ObservePeerExpectation(ZLinkAutoConnectTarget target)
    {
        if (target.NodeRid.IsEmpty) return;
        Node.SetPeerExpectation(
            target.NodeRid,
            target.Endpoint,
            ZLinkTransportSecurityIdentity.ToAdmissionIdentity(
                target.SecurityIdentity),
            target.LifecycleGeneration);
    }

    internal void ForgetPeerExpectation(ZLinkAutoConnectTarget target)
    {
        if (target.NodeRid.IsEmpty) return;
        Node.RemovePeerExpectation(
            target.NodeRid,
            target.Endpoint);
    }

    internal void ObserveRequestSourceFence(ZLinkAutoConnectTarget target)
    {
        if (target.NodeRid.IsEmpty || target.LifecycleGeneration == 0
            || string.IsNullOrWhiteSpace(target.OwnerId)
            || target.OwnerLeaseGeneration <= 0)
            return;
        if (Node is IZLinkBackendRequestSourceFenceObserver observer)
            observer.ObserveRequestSourceFence(
                new ZLinkServiceWireCodec.RequestSourceFence(
                    target.OwnerId,
                    checked((ulong)target.OwnerLeaseGeneration),
                    target.NodeRid,
                    target.LifecycleGeneration));
    }

    public void DisconnectPeerLifetime(RoutingId peerRid, ulong lifecycleGeneration)
        => Node.DisconnectPeerLifetime(peerRid, lifecycleGeneration);

    public bool DisconnectPeerAuto(string endpoint)
        => _peerConnector.DisconnectPeerAuto(endpoint);

    internal bool DisconnectPeerAuto(RoutingId peerRid, string endpoint)
        => _peerConnector.DisconnectPeerAuto(peerRid, endpoint);

    internal bool DisconnectPeerBeforeAdmission(
        ZLinkAutoConnectTarget target)
        => _peerConnector.DisconnectPeerBeforeAdmission(
            target.NodeRid,
            target.Endpoint,
            target.LifecycleGeneration);

    private async ValueTask<ZLinkEntrySpotActivation?> CreateEntrySpotActivationAsync(
        IZLinkBackendSpot entrySpot)
    {
        if (Registration.EntrySpotType is null) return null;

        var scope = _services.CreateAsyncScope();
        ZLinkEntrySpotActivation? activation = null;
        try
        {
            activation = new ZLinkEntrySpotActivation(
                _runtime,
                _services,
                scope,
                entrySpot,
                EntrySpotId,
                Registration.EntrySpotType,
                Node.RoutingId,
                Registration.SpotNodeName,
                Registration.SpotMeshChannelName ?? Registration.SpotNodeName,
                _frameworkRegistration.DefaultRequestTimeout,
                EntryOutbound,
                _timerScheduler);
            activation.InitializeRuntimeResources(_completionAdmission);
            foreach (var handler in _frameworkRegistration.ScannedHandlerCatalog.SpotHandlers)
                await activation.ApplyScannedHandlerAsync(handler, _stopSource.Token)
                    .ConfigureAwait(false);

            activation.Configure();
            await activation.InitializeAsync(_stopSource.Token).ConfigureAwait(false);
            return activation;
        }
        catch (Exception initializationFailure)
        {
            try
            {
                if (activation is null)
                    await scope.DisposeAsync().ConfigureAwait(false);
                else
                    await activation.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception cleanupFailure)
            {
                throw new AggregateException(initializationFailure, cleanupFailure);
            }

            throw;
        }
    }

    // Wires inbound node-route (NodeSend/NodeRequest) and channel-membership
    // (ChannelSend/ChannelRequest) dispatch to the MeshNode builder's registered
    // handlers. Idempotent; a node with no such handlers registers no sink and its
    // node/channel records are released by the pump.
    private void WireNodeRouteDispatch()
    {
        if (_nodeRouteDispatcher is not null) return;

        _nodeRouteDispatcher = ZLinkMeshNodeRouteDispatcher.Create(
            _services,
            _frameworkRegistration,
            Registration,
            _runtime,
            _taskRunner,
            _completionAdmission,
            _nodeSubmitter);
        if (_nodeRouteDispatcher is not null)
            Node.OnNodeRoute(_nodeRouteDispatcher.Dispatch);
    }

    private bool ShouldAttachActorDispatchPump()
    {
        return Registration.Router is not null
               && Registration.ActorFactories.Count > 0;
    }

    private async ValueTask DisposeEntrySpotAsync(bool forceStop)
    {
        if (_entrySpot is null) return;

        var failures = new List<Exception>();
        if (_entrySpotActivation is { } activation)
        {
            if (!forceStop
                && Volatile.Read(ref _entrySpotLifecycleClosed) == 0)
                await CaptureAsync(() => activation.CloseAsync(CancellationToken.None)).ConfigureAwait(false);
            await CaptureAsync(activation.DisposeAsync).ConfigureAwait(false);
        }

        if (_entryOutbound is { } outbound)
        {
            await CaptureAsync(outbound.DisposeAsync).ConfigureAwait(false);
            _entryOutbound = null;
        }

        await CaptureAsync(_entrySpot.DisposeAsync).ConfigureAwait(false);
        if (Interlocked.Exchange(ref _entrySpotMetricActive, 0) != 0)
            ZLinkRuntimeMetrics.RecordSpotClosed(
                Registration.SpotNodeName,
                "entry");
        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    private async ValueTask TrackEntrySpotLocationAsync()
    {
        if (_locationLifecycle is null || string.IsNullOrWhiteSpace(EntrySpotId))
            return;

        var nodeGeneration = Node.MeshStatus().LifecycleGeneration;
        var status = await _locationLifecycle.SpotLocations.ClaimAsync(
                Registration.SpotMeshChannelName ?? Registration.SpotNodeName,
                EntrySpotId,
                _entrySpot!.LifecycleGeneration,
                Registration.EntrySpotType?.FullName,
                Node.RoutingId,
                nodeGeneration,
                ZLinkSpotKind.Entry,
                _entrySpot.LifecycleGeneration,
                deactivate: null,
                _stopSource.Token)
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"entry_spot_location_tracked node={Node.RoutingId} "
            + $"spot={EntrySpotId} spot_gen={_entrySpot.LifecycleGeneration} "
            + $"node_gen={nodeGeneration} authority_gen={_entrySpot.LifecycleGeneration} "
            + $"status={status}");
        if (status != ZLinkLocationWriteStatus.Stored)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"Entry Spot '{EntrySpotId}' could not be tracked in the location lifecycle: {status}.");
    }
}

internal sealed record ZLinkSpotMonitoringSnapshot(
    ZLinkSpotNodeStatus Status,
    IReadOnlyList<ZLinkSpotNodePeerEntry> Peers,
    IReadOnlyList<ZLinkSpotNodeSubjectEntry> Subjects);
