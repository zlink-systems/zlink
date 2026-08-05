namespace Zlink.Framework.Runtime.Actors;

internal sealed partial class ZLinkActorSessionManager
{
    public async ValueTask DestroyActorAsync(
        RoutingId entrySpotNodeRid,
        IZLinkActor actor,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(actor);

        if (!_actorSessions.TryGet(actor.Context.ActorId, out var state)) return;

        var nativeActor = await state.ExecuteLockedAsync<ZLinkBackendActorRef?>(
            () =>
            {
                if (state.Actor is null) return null;

                if (!ReferenceEquals(state.Actor, actor)) return null;

                if (state.Activation is not null)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actor.Context.ActorId}' must leave its current SPOT before destroy.");

                var actorRef = state.NativeActorRef
                               ?? throw new ZLinkFrameworkException(
                                   ZLinkFrameworkErrorKind.NotFound,
                                   $"Actor '{actor.Context.ActorId}' does not have a native Actor ref.");

                if (actorRef.NodeRid != entrySpotNodeRid)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actor.Context.ActorId}' is not owned by this Entry Spot.");

                state.BeginTeardown();
                return actorRef;
            },
            cancellationToken).ConfigureAwait(false);

        if (nativeActor is not { } actorRef) return;

        await ExecuteActorTeardownAttemptAsync(state, actorRef, CancellationToken.None)
            .ConfigureAwait(false);
    }
}
