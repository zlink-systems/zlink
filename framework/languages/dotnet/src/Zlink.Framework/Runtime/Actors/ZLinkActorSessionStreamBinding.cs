namespace Zlink.Framework.Runtime.Actors;

internal sealed partial class ZLinkActorSessionManager
{
    public async ValueTask AttachActorAsync(
        IZLinkActor actor,
        IZLinkStream stream,
        CancellationToken cancellationToken = default)
    {
        var state = _actorSessions.GetOrCreate(actor.Context.ActorId);
        BindActorContext(actor, state);
        await BindStreamAsync(state, stream, cancellationToken).ConfigureAwait(false);

        await TryBindNativeActorAsync(state, stream, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DisconnectActorAsync(
        IZLinkActor actor,
        IZLinkStream stream,
        CancellationToken cancellationToken = default)
    {
        var state = _actorSessions.GetOrCreate(actor.Context.ActorId);
        BindActorContext(actor, state);

        var shouldUnbind = await ClearStreamBindingAsync(state, stream, cancellationToken)
            .ConfigureAwait(false);

        if (!shouldUnbind) return;

        await TryUnbindNativeActorAsync(state, actor.Context.ActorId, stream, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask BindStreamAsync(
        ZLinkActorRuntimeState state,
        IZLinkStream stream,
        CancellationToken cancellationToken)
    {
        await state.ExecuteLockedAsync(
            () => state.AttachStream(stream),
            cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<bool> ClearStreamBindingAsync(
        ZLinkActorRuntimeState state,
        IZLinkStream stream,
        CancellationToken cancellationToken)
    {
        return await state.ExecuteLockedAsync(
            () => state.DetachStreamIfCurrent(stream),
            cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask TryBindNativeActorAsync(
        ZLinkActorRuntimeState state,
        IZLinkStream stream,
        CancellationToken cancellationToken)
    {
        if (getActorSpotNode() is null
            || state.NativeActorRef is not { } actorRef)
            return;

        await ZLinkNativeActorStreamBinding.BindAsync(
                stream,
                actorRef,
                runtime.Registration.DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask TryUnbindNativeActorAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        IZLinkStream stream,
        CancellationToken cancellationToken)
    {
        if (getActorSpotNode() is null
            || state.NativeActorRef is null)
            return;

        try
        {
            await ZLinkNativeActorStreamBinding.UnbindAsync(
                    stream,
                    actorId,
                    runtime.Registration.DefaultRequestTimeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZlinkException)
        {
        }
    }
}
