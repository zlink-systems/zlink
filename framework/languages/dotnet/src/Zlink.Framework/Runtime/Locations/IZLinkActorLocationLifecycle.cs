namespace Zlink.Framework.Runtime.Locations;

internal interface IZLinkActorLocationLifecycle
{
    ValueTask<ZLinkActorClaimActivation<TActor>> ExecuteActorClaimThenActivateAsync<TActor>(
        string meshName,
        string actorType,
        string actorId,
        RoutingId nodeRid,
        Func<CancellationToken, ValueTask>? deactivate,
        Func<CancellationToken, ValueTask<TActor>> activate,
        CancellationToken cancellationToken,
        ZLinkActorClaimMode claimMode = ZLinkActorClaimMode.NewOwner)
        where TActor : class;

    ValueTask PublishActorRefAsync(
        string actorId,
        ActorRef actorRef,
        CancellationToken cancellationToken = default);

    ValueTask ReleaseActorAsync(
        string actorId,
        CancellationToken cancellationToken = default);
}
