using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Service;

internal sealed class ZLinkSessionRelocationBarrierOwner(
    ZLinkFrameworkRuntime runtime,
    IZLinkLocationRepository? authorityStore) : ISessionRelocationBarrierTarget
{
    public ValueTask<ZLinkServiceWireCodec.SessionRelocationSealedRecord>
        SealAsync(
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            RoutingId authenticatedSourceNodeRid,
            CancellationToken cancellationToken)
    {
        if (seal.Coordinator.NodeRid != authenticatedSourceNodeRid)
            throw new InvalidDataException(
                "Command 42 coordinator does not match the authenticated source.");
        return runtime.SealCanonicalSessionActorRouteAsync(
            seal,
            cancellationToken);
    }

    public async ValueTask<ZLinkServiceWireCodec.SessionRelocationRoutedRecord>
        RouteAsync(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
            ZLinkSessionRelocationAuthenticatedRoute authenticatedRoute,
            CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var senderMatches = route.Route.Action
            == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                ? route.SenderRole == 2
                  && route.Route.TargetNodeRid == authenticatedRoute.NodeRid
                  && route.Route.TargetNodeGeneration
                  == authenticatedRoute.NodeGeneration
                  && route.Route.TargetAuthorityOwnerGeneration
                  == authenticatedRoute.AuthorityOwnerGeneration
                : route.SenderRole == 1
                  && route.Coordinator.NodeRid == authenticatedRoute.NodeRid
                  && route.Coordinator.NodeGeneration
                  == authenticatedRoute.NodeGeneration
                  && route.Route.CurrentAuthorityOwnerGeneration
                  == authenticatedRoute.AuthorityOwnerGeneration;
        if (!senderMatches)
            throw new InvalidDataException(
                "Command 44 sender role does not match the authenticated source.");

        return await runtime.RouteCanonicalSessionActorAsync(
                route,
                authenticatedRoute,
                token => ResolveAuthorityAsync(route, authenticatedRoute, token),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkSessionRelocationAuthenticatedRoute>
        ResolveAuthorityAsync(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
            ZLinkSessionRelocationAuthenticatedRoute authenticatedRoute,
            CancellationToken cancellationToken)
    {
        var store = authorityStore
                    ?? throw new InvalidOperationException(
                        "Session relocation route verification requires a location store.");
        var authorityRead = await store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    route.Actor.ActorId),
                cancellationToken)
            .ConfigureAwait(false);
        var expectedNodeRid = route.Route.Action
            == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                ? route.Route.TargetNodeRid
                : route.Coordinator.NodeRid;
        var expectedNodeGeneration = route.Route.Action
            == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                ? route.Route.TargetNodeGeneration
                : route.Coordinator.NodeGeneration;
        var expectedAuthorityOwnerGeneration = route.Route.Action
            == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                ? route.Route.TargetAuthorityOwnerGeneration
                : route.Route.CurrentAuthorityOwnerGeneration;
        if (authorityRead is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.ObjectGeneration
               != route.Actor.ObjectGeneration
            || found.Snapshot.AuthorityOwnerGeneration
               != expectedAuthorityOwnerGeneration
            || found.Snapshot.OwnerLeaseGeneration <= 0
            || found.Snapshot.Allocation.ObjectKind
               != ZLinkPlacementObjectKind.Actor
            || found.Snapshot.Allocation.Descriptor.Rid
               != expectedNodeRid
            || !StringComparer.Ordinal.Equals(
                found.Snapshot.Allocation.Descriptor.MeshName,
                authenticatedRoute.MeshName)
            || found.Snapshot.Allocation.DescriptorLifecycleGeneration
               != expectedNodeGeneration
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                found.Snapshot.Payload.Span,
                out var actorAuthority)
            || actorAuthority.State != ZLinkActorAuthorityState.Ready
            || !StringComparer.Ordinal.Equals(
                actorAuthority.ActorId,
                route.Actor.ActorId)
            || !StringComparer.Ordinal.Equals(
                actorAuthority.OwnerId,
                found.Snapshot.OwnerId)
            || actorAuthority.OwnerLeaseGeneration
               != checked((ulong)found.Snapshot.OwnerLeaseGeneration)
            || !StringComparer.Ordinal.Equals(
                actorAuthority.MeshName,
                authenticatedRoute.MeshName)
            || actorAuthority.NodeRid != expectedNodeRid
            || actorAuthority.NodeGeneration != expectedNodeGeneration)
        {
            throw new InvalidDataException(
                "Command 44 does not match the exact current Actor authority.");
        }

        return authenticatedRoute with
        {
            AuthorityOwnerGeneration = found.Snapshot.AuthorityOwnerGeneration,
            OwnerLeaseGeneration = checked(
                (ulong)found.Snapshot.OwnerLeaseGeneration)
        };
    }
}
