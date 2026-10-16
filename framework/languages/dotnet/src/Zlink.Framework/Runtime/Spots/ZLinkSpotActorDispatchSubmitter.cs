namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorDispatchSubmitter(
    ZLinkFrameworkRuntime runtime,
    ZLinkSpotSerialExecutor serial,
    ZLinkSpotActorPacketDispatcher dispatcher
)
{
    public async ValueTask Async(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        ZLinkSpotExecutionRelocationSeal? relocationSeal,
        ZLinkSpotRelocationActorQueueReservation? queueReservation,
        CancellationToken cancellationToken,
        RoutingId? sourceSessionRid
    )
    {
        var ownedPayload = payload.Copy();

        try
        {
            using var operation = (
                await AdmitActorOperationAsync(queueReservation).ConfigureAwait(false)
            ).Enter();
            var state = new ActorDispatchState(
                dispatcher,
                actor,
                runtimeState,
                header,
                ownedPayload,
                sourceSessionRid
            );
            await SubmitAsync(
                    runtimeState.ActorId,
                    DispatchAsync,
                    state,
                    relocationSeal,
                    queueReservation,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch
        {
            ownedPayload.Dispose();
            throw;
        }
    }

    public async ValueTask<ZLinkActorReply> SubmitForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        ZLinkSpotExecutionRelocationSeal? relocationSeal,
        ZLinkSpotRelocationActorQueueReservation? queueReservation,
        CancellationToken cancellationToken,
        RoutingId? sourceSessionRid
    )
    {
        var ownedPayload = payload.Copy();

        try
        {
            using var operation = (
                await AdmitActorOperationAsync(queueReservation).ConfigureAwait(false)
            ).Enter();
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_serial_submit_begin actor={runtimeState.ActorId} correlation_id={header.CorrelationId}"
            );
            var state = new ActorReplyDispatchState(
                dispatcher,
                actor,
                runtimeState,
                header,
                ownedPayload,
                sourceSessionRid
            );
            await SubmitAsync(
                    runtimeState.ActorId,
                    DispatchForReplyAsync,
                    state,
                    relocationSeal,
                    queueReservation,
                    cancellationToken
                )
                .ConfigureAwait(false);

            var reply =
                state.Reply
                ?? throw new InvalidOperationException(
                    $"SPOT actor packet reply for '{header.Name}' was null."
                );
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_serial_submit_completed actor={runtimeState.ActorId} correlation_id={header.CorrelationId}"
            );
            return reply;
        }
        catch
        {
            ownedPayload.Dispose();
            throw;
        }
    }

    private ValueTask<ZLinkFrameworkRuntime.ZLinkRuntimeOperationAdmission> AdmitActorOperationAsync(
        ZLinkSpotRelocationActorQueueReservation? reservation
    ) =>
        // The validated replay reservation already owns its accepted queue turn.
        // Filling it does not start another host operation after the seal.
        reservation is not null
            ? ValueTask.FromResult(default(ZLinkFrameworkRuntime.ZLinkRuntimeOperationAdmission))
            : runtime.TryAdmitInboundOperationAsync(countAsRequest: false);

    private ValueTask SubmitAsync<TState>(
        string actorId,
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> dispatch,
        TState state,
        ZLinkSpotExecutionRelocationSeal? relocationSeal,
        ZLinkSpotRelocationActorQueueReservation? queueReservation,
        CancellationToken cancellationToken
    ) =>
        // A validated replay reservation reaches this submitter only from
        // ZLinkActorRuntimeState.ExecuteDispatchAsync, which already runs
        // the replay inside that reserved queue turn.
        queueReservation is not null
            ? dispatch(null!, state, cancellationToken)
        : relocationSeal is not null
            ? serial.ExecuteRelocationActorAsync(
                relocationSeal,
                actorId,
                dispatch,
                state,
                cancellationToken
            )
        : serial.ExecuteActorAsync(actorId, dispatch, state, cancellationToken);

    private static async ValueTask DispatchAsync(
        ZLinkSpotActivation _,
        ActorDispatchState state,
        CancellationToken cancellationToken
    )
    {
        using var currentPayload = state.Payload;
        // The Actor mailbox owns the outer turn, but the Spot serial executor
        // deliberately runs application callbacks on its own execution
        // context. Carry the exact Actor ownership across that queue boundary
        // so lifecycle calls such as DestroyActorAsync can defer native
        // terminal work until the outer delivery returns.
        using var dispatch = state.RuntimeState.EnterForwardedDispatchExecution();
        await state
            .Dispatcher.DispatchAsync(
                state.Actor,
                state.RuntimeState,
                state.Header,
                currentPayload,
                cancellationToken,
                state.SourceSessionRid
            )
            .ConfigureAwait(false);
    }

    private static async ValueTask DispatchForReplyAsync(
        ZLinkSpotActivation _,
        ActorReplyDispatchState state,
        CancellationToken cancellationToken
    )
    {
        using var currentPayload = state.Payload;
        using var dispatch = state.RuntimeState.EnterForwardedDispatchExecution();
        state.Reply = await state
            .Dispatcher.DispatchForReplyAsync(
                state.Actor,
                state.RuntimeState,
                state.Header,
                currentPayload,
                cancellationToken,
                state.SourceSessionRid
            )
            .ConfigureAwait(false);
    }

    private sealed class ActorDispatchState(
        ZLinkSpotActorPacketDispatcher dispatcher,
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        RoutingId? sourceSessionRid
    )
    {
        public ZLinkSpotActorPacketDispatcher Dispatcher { get; } = dispatcher;

        public IZLinkActor Actor { get; } = actor;

        public ZLinkActorRuntimeState RuntimeState { get; } = runtimeState;

        public ZlinkStreamHeader Header { get; } = header;

        public Message Payload { get; } = payload;

        public RoutingId? SourceSessionRid { get; } = sourceSessionRid;
    }

    private sealed class ActorReplyDispatchState(
        ZLinkSpotActorPacketDispatcher dispatcher,
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        RoutingId? sourceSessionRid
    )
    {
        public ZLinkSpotActorPacketDispatcher Dispatcher { get; } = dispatcher;

        public IZLinkActor Actor { get; } = actor;

        public ZLinkActorRuntimeState RuntimeState { get; } = runtimeState;

        public ZlinkStreamHeader Header { get; } = header;

        public Message Payload { get; } = payload;

        public RoutingId? SourceSessionRid { get; } = sourceSessionRid;

        public ZLinkActorReply? Reply { get; set; }
    }
}
