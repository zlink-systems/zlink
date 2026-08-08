using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Locations;

internal interface IZLinkActorLocationLifecycle
{
    ValueTask<ZLinkActorClaimActivation<TActor>> ExecuteActorClaimThenActivateAsync<TActor>(
        ZLinkMeshName meshName,
        string actorType,
        ZLinkActorId actorId,
        RoutingId nodeRid,
        Func<CancellationToken, ValueTask>? deactivate,
        Func<CancellationToken, ValueTask<TActor>> activate,
        CancellationToken cancellationToken,
        ZLinkActorClaimMode claimMode = ZLinkActorClaimMode.NewOwner)
        where TActor : class;

    ValueTask PublishActorRefAsync(
        ZLinkActorId actorId,
        ActorRef actorRef,
        CancellationToken cancellationToken = default);

    ValueTask ReleaseActorAsync(
        ZLinkActorId actorId,
        CancellationToken cancellationToken = default);
}
