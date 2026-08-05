namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal InstanceSpotIntentAddress ResolveInstanceSpotIntent(
        InstanceSpotIntentAddress address)
    {
        var source = ResolveActorCreationSource(
            string.IsNullOrEmpty(address.MeshName) ? null : address.MeshName);
        var meshName = source.Registration.SpotMeshChannelName
                       ?? source.Registration.SpotNodeName;
        if (!string.IsNullOrEmpty(address.InstanceSpotType))
            return address with { MeshName = meshName };
        var types = source.Registration.InstanceSpotFactories.Keys.ToArray();
        return types.Length switch
        {
            1 => address with
            {
                MeshName = meshName,
                InstanceSpotType = types[0]
            },
            0 => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Mesh '{meshName}' has no registered Instance Spot type."),
            _ => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "InstanceSpot(instanceSpotType) is required when a Mesh registers multiple Instance Spot types.")
        };
    }

    internal ZLinkSpotNodeRuntime ResolveActorCreationSource(string? meshName)
    {
        var state = _state
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Framework runtime is not started.");
        var candidates = state.SpotNodes.Values
            .Where(node => node.Registration.ObjectRoleSelected
                && (meshName is null
                    || string.Equals(
                        node.Registration.SpotMeshChannelName
                        ?? node.Registration.SpotNodeName,
                        meshName,
                        StringComparison.Ordinal)))
            .ToArray();
        if (candidates.Length == 1)
            return candidates[0];
        throw new ZLinkFrameworkException(
            candidates.Length == 0
                ? meshName is null
                    ? ZLinkFrameworkErrorKind.NotConfigured
                    : ZLinkFrameworkErrorKind.NotFound
                : ZLinkFrameworkErrorKind.InvalidOperation,
            candidates.Length == 0
                ? meshName is null
                    ? "No object client MeshNode is registered."
                    : $"Object MeshNode '{meshName}' is not registered."
                : "More than one object client MeshNode is registered; InMesh is required.");
    }
    public IZLinkSpotCreateCall Create(string spotType)
    {
        var stableType = RequireSpotType(spotType);
        return new ZLinkSpotCreateCall(
            Registration.DefaultRequestTimeout,
            (mesh, request, timeout, cancellation) =>
                SubmitUserSpotAsync(
                    stableType,
                    Guid.NewGuid().ToString("D"),
                    mesh,
                    request,
                    timeout,
                    false,
                    cancellation));
    }

    public IZLinkSpotGetOrCreateCall GetOrCreate(
        string spotId,
        string spotType)
    {
        var stableType = RequireSpotType(spotType);
        ZLinkSpotId.RequireCallerProvided(spotId, nameof(spotId));
        return new ZLinkSpotGetOrCreateCall(
            Registration.DefaultRequestTimeout,
            (mesh, request, timeout, cancellation) =>
                SubmitUserSpotAsync(
                    stableType,
                    spotId,
                    mesh,
                    request,
                    timeout,
                    true,
                    cancellation));
    }

    private async ValueTask<ZLinkSpotCreateResult> SubmitUserSpotAsync(
        string stableType,
        string spotId,
        string? meshName,
        ZLinkMessage request,
        TimeSpan timeout,
        bool joinExisting,
        CancellationToken cancellationToken)
    {
        using var operation = EnterOperation();
        _drainAdmission.RequireSpotAdmission();
        return await _spots.CreateByStableTypeAsync(
                GetOrStartState(),
                stableType,
                spotId,
                meshName,
                request,
                timeout,
                joinExisting,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal ZLinkSpotPublisherBundle GetSpotPublisherBundle(string channelName)
    {
        var meshName = ResolveRouteMeshForChannel(channelName);
        return _spots.GetPublisherBundle(GetOrStartState(), meshName);
    }

    public ValueTask<ZLinkSpotCreateResult> CreateAsync<TSpot>(
        CancellationToken cancellationToken = default)
        where TSpot : IZLinkSpot
    {
        return CreateAsync<TSpot>(ZLinkMessage.Empty, cancellationToken);
    }

    public async ValueTask<ZLinkSpotCreateResult> CreateAsync<TSpot>(
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
        where TSpot : IZLinkSpot
    {
        using var operation = EnterOperation();
        _drainAdmission.RequireSpotAdmission();
        return await _spots.CreateAsync(GetOrStartState(), typeof(TSpot), request, cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkSpotCreateResult> GetOrCreateAsync<TSpot>(
        string spotId,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
        where TSpot : IZLinkSpot
    {
        ZLinkSpotId.RequireCallerProvided(spotId, nameof(spotId));
        using var operation = EnterOperation();
        _drainAdmission.RequireSpotAdmission();
        return await _spots.GetOrCreateAsync(
                GetOrStartState(), typeof(TSpot), spotId, request, cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask<ZLinkSpotCreateResult> GetOrCreateAsync<TSpot>(
        string spotId,
        CancellationToken cancellationToken = default)
        where TSpot : IZLinkSpot
    {
        return GetOrCreateAsync<TSpot>(spotId, ZLinkMessage.Empty, cancellationToken);
    }

    public async ValueTask<SpotRef?> FindAsync(
        string spotId,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperationalRead();
        return await _spots.ResolveAsync(spotId, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkSpotActivation?> GetSpotActivationByRidAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        using var operation = EnterOperation();
        var state = await GetStartedStateForRoutingAsync(cancellationToken)
            .ConfigureAwait(false);
        return _spots.GetActivationBySpotId(state, spotId);
    }

    public async ValueTask<IReadOnlyList<ZLinkSpotInfo>> ListAsync(
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        return await _spots.ListAsync(GetOrStartState(), cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<bool> CloseAsync(
        SpotRef spot,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        return await _spots.CloseAsync(GetOrStartState(), spot, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<bool> CloseCurrentSpotAsync(
        string spotId,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        return await _spots.CloseLocalByIdAsync(GetOrStartState(), spotId, cancellationToken)
            .ConfigureAwait(false);
    }

    private static string RequireSpotType(string value)
    {
        if (string.IsNullOrWhiteSpace(value)
            || System.Text.Encoding.UTF8.GetByteCount(value) > byte.MaxValue
            || value.Contains('\0'))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Spot type must be 1..255 UTF-8 bytes without NUL.");
        return value;
    }

    internal IZLinkBackendSpotNode? GetActorSpotNode()
    {
        return GetActorSpotNodeRuntime()?.Node;
    }

    internal void SetActivationConcurrency(string meshName, int active)
    {
        _autoConnect?.SetLocalActivationConcurrency(meshName, active);
    }

    internal ZLinkSpotNodeRuntime? GetActorSpotNodeRuntime(
        string? actorType = null)
    {
        if (actorType is not null)
            return _state?.SpotNodes.Values
                .SingleOrDefault(node =>
                    node.Registration.Router is not null
                    && node.Registration.ActorFactories.ContainsKey(actorType));

        return _state?.SpotNodes.Values
            .SingleOrDefault(static node =>
                node.Registration.Router is not null
                && node.Registration.ActorFactories.Count > 0);
    }

    internal IZLinkBackendSpotNode GetActorClientSpotNode()
    {
        return GetActorClientSpotNodeRuntime().Node;
    }

    /// <summary>Any router-capable node, or null before startup — the
    /// bound-session relay planes live on the router plane even on hosts
    /// without local actor factories (a session host binding remote actors).</summary>
    internal IZLinkBackendSpotNode? GetRouterSpotNodeOrNull()
    {
        return _state?.SpotNodes.Values
            .FirstOrDefault(static node => node.Registration.Router is not null)
            ?.Node;
    }

    /// <summary>The registered MeshNode for a physical mesh. ChannelName
    /// select-one calls (IZLinkRouteClient) submit through this node's entry
    /// spot so weight, ready and drain admission stay Core-owned (spec 11 §3).</summary>
    internal ZLinkSpotNodeRuntime GetMeshNodeRuntime(string meshName)
    {
        var state = GetOrStartState();
        lock (state.SyncRoot)
        {
            return state.SpotNodes.TryGetValue(meshName, out var nodeRuntime)
                   ? nodeRuntime
                   : throw new ZLinkConfigurationException(
                       $"RouteMesh '{meshName}' is not registered.");
        }
    }

    internal ZLinkSpotNodeRuntime GetActorClientSpotNodeRuntime()
    {
        var state = GetOrStartState();
        lock (state.SyncRoot)
        {
            return state.SpotNodes.Values
                       .FirstOrDefault(static node => node.Registration.Router is not null)
                   ?? throw new ZLinkConfigurationException(
                       "Actor client requires a router-capable SPOT node.");
        }
    }

    internal ValueTask<bool> TrySubmitEntrySpotActorAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken = default)
    {
        return ExecuteOperationAsync(() =>
        {
            var state = GetOrStartState();
            return _spots.EntrySpotActors.TryAsync(
                state, actor, header, payload, cancellationToken);
        });
    }

    internal ValueTask<EntrySpotActorReplyDispatchResult> TrySubmitEntrySpotActorForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        bool callerOwnsDispatchTurn,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        return ExecuteOperationAsync(() =>
        {
            var state = GetOrStartState();
            return _spots.EntrySpotActors.TrySubmitForReplyAsync(
                state,
                actor,
                runtimeState,
                header,
                payload,
                callerOwnsDispatchTurn,
                relocationReplay,
                cancellationToken);
        });
    }

    internal async ValueTask NotifyEntrySpotActorJoinedAsync(
        IZLinkActor actor,
        RoutingId? targetNodeRid = null,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        if (_state is null) return;

        await _spots.EntrySpotActors.NotifyJoinedAsync(
                _state,
                actor,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkActorCreateResponse> NotifyEntrySpotActorCreatedAsync(
        IZLinkActor actor,
        RoutingId? targetNodeRid = null,
        CancellationToken cancellationToken = default)
    {
        return await NotifyEntrySpotActorCreatedAsync(
                actor,
                ZLinkMessage.Empty,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkActorCreateResponse> NotifyEntrySpotActorCreatedAsync(
        IZLinkActor actor,
        ZLinkMessage createRequest,
        RoutingId? targetNodeRid = null,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        if (_state is null) return ZLinkActorCreateResponse.Accept();

        return await _spots.EntrySpotActors.NotifyCreatedAsync(
                _state,
                actor,
                createRequest,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask NotifyEntrySpotActorLeftAsync(
        IZLinkActor actor,
        RoutingId? targetNodeRid = null,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        if (_state is null) return;

        await _spots.EntrySpotActors.NotifyLeftAsync(
                _state,
                actor,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<bool> TryNotifyEntrySpotActorDisconnectedAsync(
        IZLinkActor actor,
        RoutingId? targetNodeRid = null,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        if (_state is null) return false;

        return await _spots.EntrySpotActors.TryNotifyDisconnectedAsync(
                _state,
                actor,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<bool> TryNotifyJoinedSpotActorDisconnectedAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        using var operation = EnterOperation();
        if (_state is null) return false;

        if (_actorSessionManager.TryGetCreatedActorState(actorId, out var actorState)
            && actorState is { Actor: { } actor, LiveActivation: { } activation })
        {
            await activation.NotifyActorDisconnectedAsync(actor, cancellationToken)
                .ConfigureAwait(false);
            return true;
        }

        return await _spots.TryNotifyJoinedSpotActorDisconnectedAsync(
                _state,
                actorId,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal ZLinkSpotMonitoringSnapshot GetSpotMonitoringSnapshot(string spotNodeName)
    {
        return ExecuteOperation(() => _spots.GetMonitoringSnapshot(GetOrStartState(), spotNodeName));
    }

    internal ZLinkSpotNodeRuntime GetSpotNodeRuntime(string spotNodeName)
    {
        return ExecuteOperation(() =>
        {
            var state = GetOrStartState();
            return state.SpotNodes.TryGetValue(spotNodeName, out var node)
                ? node
                : throw new ZLinkConfigurationException($"SPOT node '{spotNodeName}' is not registered.");
        });
    }
}
