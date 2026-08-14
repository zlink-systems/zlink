using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Service;

internal sealed class ZLinkSessionRelocationBarrierOwner(
    ZLinkFrameworkRuntime runtime) : ISessionRelocationBarrierTarget
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

    public ValueTask RouteAsync(
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

        return runtime.RouteCanonicalSessionActorAsync(
            route,
            authenticatedRoute,
            cancellationToken);
    }
}
