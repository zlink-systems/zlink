namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorDispatchSubmitter(
    ZLinkSpotSerialExecutor serial,
    ZLinkSpotActorPacketDispatcher dispatcher)
{
    public async ValueTask Async(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload,
        ZLinkSpotExecutionRelocationSeal? relocationSeal,
        ZLinkSpotRelocationActorQueueReservation? queueReservation,
        CancellationToken cancellationToken)
    {
        var ownedPayload = payload.Copy();

        try
        {
            var state = new ActorDispatchState(
                dispatcher,
                actor,
                runtimeState,
                header,
                ownedPayload);
            var execution = queueReservation is not null
                ? queueReservation.ExecuteAsync(
                    runtimeState.ActorId,
                    ct => DispatchAsync(
                        null!,
                        state,
                        ct),
                    cancellationToken)
                : relocationSeal is not null
                ? serial.ExecuteRelocationActorAsync(
                    relocationSeal,
                    runtimeState.ActorId,
                    DispatchAsync,
                    state,
                    cancellationToken)
                : serial.ExecuteActorAsync(
                    runtimeState.ActorId,
                    DispatchAsync,
                    state,
                    cancellationToken);
            await execution.ConfigureAwait(false);
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
        CancellationToken cancellationToken)
    {
        var ownedPayload = payload.Copy();

        try
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_serial_submit_begin actor={runtimeState.ActorId} "
                + $"correlation_id={header.CorrelationId}");
            var state = new ActorReplyDispatchState(dispatcher, actor, runtimeState, header, ownedPayload);
            var execution = queueReservation is not null
                ? queueReservation.ExecuteAsync(
                    runtimeState.ActorId,
                    ct => DispatchForReplyAsync(
                        null!,
                        state,
                        ct),
                    cancellationToken)
                : relocationSeal is not null
                ? serial.ExecuteRelocationActorAsync(
                    relocationSeal,
                    runtimeState.ActorId,
                    DispatchForReplyAsync,
                    state,
                    cancellationToken)
                : serial.ExecuteActorAsync(
                    runtimeState.ActorId,
                    DispatchForReplyAsync,
                    state,
                    cancellationToken);
            await execution.ConfigureAwait(false);

            var reply = state.Reply
                         ?? throw new InvalidOperationException(
                       $"SPOT actor packet reply for '{header.Name}' was null.");
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_serial_submit_completed actor={runtimeState.ActorId} "
                + $"correlation_id={header.CorrelationId}");
            return reply;
        }
        catch
        {
            ownedPayload.Dispose();
            throw;
        }
    }

    private static async ValueTask DispatchAsync(
        ZLinkSpotActivation _,
        ActorDispatchState state,
        CancellationToken cancellationToken)
    {
        using var currentPayload = state.Payload;
        await state.Dispatcher.DispatchAsync(
                state.Actor,
                state.RuntimeState,
                state.Header,
                currentPayload,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static async ValueTask DispatchForReplyAsync(
        ZLinkSpotActivation _,
        ActorReplyDispatchState state,
        CancellationToken cancellationToken)
    {
        using var currentPayload = state.Payload;
        state.Reply = await state.Dispatcher.DispatchForReplyAsync(
                state.Actor,
                state.RuntimeState,
                state.Header,
                currentPayload,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private sealed class ActorDispatchState(
        ZLinkSpotActorPacketDispatcher dispatcher,
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload)
    {
        public ZLinkSpotActorPacketDispatcher Dispatcher { get; } = dispatcher;

        public IZLinkActor Actor { get; } = actor;

        public ZLinkActorRuntimeState RuntimeState { get; } = runtimeState;

        public ZlinkStreamHeader Header { get; } = header;

        public Message Payload { get; } = payload;
    }

    private sealed class ActorReplyDispatchState(
        ZLinkSpotActorPacketDispatcher dispatcher,
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message payload)
    {
        public ZLinkSpotActorPacketDispatcher Dispatcher { get; } = dispatcher;

        public IZLinkActor Actor { get; } = actor;

        public ZLinkActorRuntimeState RuntimeState { get; } = runtimeState;

        public ZlinkStreamHeader Header { get; } = header;

        public Message Payload { get; } = payload;

        public ZLinkActorReply? Reply { get; set; }
    }
}
