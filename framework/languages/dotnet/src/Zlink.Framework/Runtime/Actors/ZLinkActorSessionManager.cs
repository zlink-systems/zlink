namespace Zlink.Framework.Runtime.Actors;

internal sealed partial class ZLinkActorSessionManager(
    ZLinkFrameworkRuntime runtime,
    IServiceProvider services,
    Func<IZLinkBackendSpotNode?> getActorSpotNode,
    ZLinkLocationLifecycle? locationLifecycle,
    IZLinkBoundSessionService boundSessionService,
    Func<string, ZLinkActivationConcurrencyAdmission?>? getActivationAdmission = null)
{
    private readonly ZLinkActorSessionRegistry _actorSessions = new(
        services,
        runtime.LogActorHandoff,
        runtime.Registration.DefaultRequestTimeout
        + runtime.Registration.DefaultRequestTimeout);

    private ZLinkLocationLifecycle? LocationLifecycle { get; } = locationLifecycle;

    private ZLinkActorCreationCoordinator? _actorCreationInitialized;
    private ZLinkActorDispatchRouter? _dispatchRouterInitialized;

    private ZLinkActorCreationCoordinator ActorCreation => _actorCreationInitialized
        ??= new ZLinkActorCreationCoordinator(
            runtime,
            services,
            getActorSpotNode,
            LocationLifecycle?.ActorOwnership,
            EnsureActorContext,
            BindActorContext,
            ExecuteActorTeardownAttemptAsync,
            getActivationAdmission);

    private ZLinkActorDispatchRouter DispatchRouter => _dispatchRouterInitialized
        ??= new ZLinkActorDispatchRouter(runtime, _actorSessions, BindActorContext);

    internal ZLinkActorRuntimeState[] SnapshotStates() => _actorSessions.Snapshot();

    public async ValueTask<CreateActorResult> CreateAndBindActorAsync(
        string actorId,
        string actorType,
        CancellationToken cancellationToken = default)
    {
        return await CreateAndBindActorAsync(
                actorId,
                actorType,
                ZLinkMessage.Empty,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<CreateActorResult> CreateAndBindActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken = default)
    {
        return await CreateAndBindActorAsync(
                actorId,
                actorType,
                createRequest,
                false,
                ZLinkActorClaimMode.NewOwner,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<CreateActorResult> CreateAndBindActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        ZLinkActorClaimMode claimMode,
        CancellationToken cancellationToken = default)
    {
        return await CreateAndBindActorAsync(
                actorId,
                actorType,
                createRequest,
                false,
                claimMode,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<CreateActorResult> RelocateAndBindActorAsync(
        string actorId,
        string actorType,
        ZLinkObjectRelocationRegistration relocation,
        ReadOnlyMemory<byte> relocationState,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkActorClaimMode claimMode,
        bool publishActorRef,
        CancellationToken cancellationToken = default)
    {
        var state = _actorSessions.GetOrCreate(actorId);
        return await ActorCreation.RelocateAndBindActorAsync(
                state,
                actorId,
                actorType,
                relocation,
                relocationState,
                objectGeneration,
                authorityOwnerGeneration,
                claimMode,
                publishActorRef,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<CreateActorResult> PrepareReservedActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        var state = _actorSessions.GetOrCreate(actorId);
        // A source that completed a handoff keeps its retired native ref and
        // closed activation until the next local materialization. A new
        // durable object generation on this node must retire that old source
        // before the reserved Actor factory can use the state again.
        if (state.Actor is null && state.RetiredLocalActorRef is not null)
            await PrepareForTransferredActivationAsync(state, cancellationToken)
                .ConfigureAwait(false);
        return await ActorCreation.PrepareReservedActorAsync(
                state,
                actorId,
                actorType,
                createRequest,
                objectGeneration,
                authorityOwnerGeneration,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal void PublishReservedActor(string actorId)
    {
        if (_actorSessions.TryGet(actorId, out var state))
            state.PublishReservedCreation();
    }

    public async ValueTask<CreateActorResult> CreateActorAsync(
        string actorId,
        string actorType,
        CancellationToken cancellationToken = default)
    {
        return await CreateActorAsync(
                actorId,
                actorType,
                ZLinkMessage.Empty,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<CreateActorResult> CreateActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken = default)
    {
        return await CreateAndBindActorAsync(
                actorId,
                actorType,
                createRequest,
                true,
                ZLinkActorClaimMode.NewOwner,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask<IZLinkActor?> FindActorAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(
            _actorSessions.TryGet(actorId, out var state)
                && !state.IsDispatchBlocked
                ? state.Actor
                : null);
    }

    public bool TryGetCreatedActor(
        string actorId,
        string actorType,
        out IZLinkActor actor)
    {
        actor = null!;
        if (!TryGetCreatedActorState(actorId, actorType, out var state)) return false;

        actor = state.Actor!;
        return true;
    }

    public bool TryGetCreatedActorState(
        string actorId,
        out ZLinkActorRuntimeState state)
    {
        state = null!;
        if (!_actorSessions.TryGet(actorId, out var existingState)
            || existingState.IsDispatchBlocked
            || existingState.Actor is null)
            return false;

        state = existingState;
        return true;
    }

    public bool TryGetCreatedActorState(
        string actorId,
        string actorType,
        out ZLinkActorRuntimeState state)
    {
        state = null!;
        if (!_actorSessions.TryGet(actorId, out var existingState)
            || existingState.IsDispatchBlocked
            || existingState.Actor is null)
            return false;

        if (existingState.ActorType is not null
            && !string.Equals(existingState.ActorType, actorType, StringComparison.Ordinal))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"Actor '{actorId}' already uses actor type '{existingState.ActorType}', not '{actorType}'.");

        state = existingState;
        return true;
    }

    private async ValueTask<CreateActorResult> CreateAndBindActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        bool failIfExists,
        ZLinkActorClaimMode claimMode,
        CancellationToken cancellationToken)
    {
        var state = _actorSessions.GetOrCreate(actorId);
        return await ActorCreation.CreateAndBindActorAsync(
                state,
                actorId,
                actorType,
                createRequest,
                failIfExists,
                claimMode,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask SubmitActorByIdAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken = default)
    {
        await DispatchRouter.SubmitByIdAsync(actorId, header, payload, cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorReply> SubmitActorForReplyAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        return await DispatchRouter.SubmitForReplyAsync(
                actorId,
                header,
                payload,
                relocationReplay,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask SubmitActorAsync(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        await DispatchRouter.Async(
                actor,
                header,
                payload,
                relocationReplay,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask NotifyDisconnectedByIdAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        await DispatchRouter.NotifyDisconnectedByIdAsync(actorId, cancellationToken)
            .ConfigureAwait(false);
    }

    private ZLinkActorContext BindActorContext(
        IZLinkActor actor,
        ZLinkActorRuntimeState state)
    {
        var assignedActor = state.BindActorInstance(actor);

        var context = EnsureActorContext(state);
        if (!ReferenceEquals(actor.Context, context))
            throw new InvalidOperationException(
                $"Actor '{actor.Context.ActorId}' must expose the context provided by its factory.");

        if (state.TryBeginActorConfiguration())
        {
            try
            {
                actor.Configure();
            }
            catch
            {
                state.RollBackActorConfiguration(assignedActor);

                throw;
            }
        }

        return context;
    }

    private ZLinkActorContext EnsureActorContext(ZLinkActorRuntimeState state)
    {
        var actorType = state.ActorType
                        ?? throw new InvalidOperationException(
                            $"Actor '{state.ActorId}' does not have a registered actor type.");
        var meshName = ZLinkActorDrainCoordinator.ResolveMeshName(
                           runtime.Registration,
                           actorType)
                       ?? throw new InvalidOperationException(
                           $"Actor '{state.ActorId}' does not belong to a registered RouteMesh.");
        var objectGeneration = state.NativeActorRef?.Generation
                               ?? throw new InvalidOperationException(
                                   $"Actor '{state.ActorId}' does not have an object generation.");
        return state.GetOrCreateContext(() => new ZLinkActorContext(
            runtime,
            state,
            meshName,
            objectGeneration,
            state.SpotId,
            boundSessionService));
    }

    public ZLinkActorRuntimeState GetOrCreateState(string actorId)
    {
        return _actorSessions.GetOrCreate(actorId);
    }

    internal bool TryGetState(
        string actorId,
        out ZLinkActorRuntimeState state)
    {
        return _actorSessions.TryGet(actorId, out state!);
    }

    internal ValueTask ResetGenerationAsync(
        CancellationToken cancellationToken = default,
        Action<Exception>? detachedCleanupFailure = null)
    {
        return _actorSessions.ResetGenerationAsync(
            cancellationToken,
            detachedCleanupFailure);
    }

    internal ValueTask ResetBoundSessionGenerationAsync() =>
        boundSessionService.ResetAsync();
}
