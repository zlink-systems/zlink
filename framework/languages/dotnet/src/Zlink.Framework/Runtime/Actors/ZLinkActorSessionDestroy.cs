using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Actors;

internal sealed partial class ZLinkActorSessionManager
{
    public async ValueTask DestroyActorAsync(
        RoutingId entrySpotNodeRid,
        IZLinkActor actor,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentNullException.ThrowIfNull(actor);

        if (
            !_actorSessions.TryGet(
                ZLinkActorId.FromBoundary(actor.Context.ActorId, nameof(actor)),
                out var state
            )
        )
            return;

        var teardown = await state
            .ExecuteLockedAsync<(
                ZLinkBackendActorRef Actor,
                ZLinkActorTeardownOperation Operation
            )?>(
                () =>
                {
                    if (state.Actor is null)
                        return null;

                    if (!ReferenceEquals(state.Actor, actor))
                        return null;

                    if (state.Activation is not null)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{actor.Context.ActorId}' must leave its current SPOT before destroy."
                        );

                    var actorRef =
                        state.NativeActorRef
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{actor.Context.ActorId}' does not have a native Actor ref."
                        );

                    if (actorRef.NodeRid != entrySpotNodeRid)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{actor.Context.ActorId}' is not owned by this Entry Spot."
                        );

                    return (actorRef, state.BeginOrJoinTeardownAttemptOnLane(actorRef));
                },
                cancellationToken
            )
            .ConfigureAwait(false);

        if (teardown is not { } transaction)
            return;

        await ExecuteActorTeardownAttemptAsync(
                state,
                transaction.Actor,
                transaction.Operation,
                CancellationToken.None
            )
            .ConfigureAwait(false);
    }
}
