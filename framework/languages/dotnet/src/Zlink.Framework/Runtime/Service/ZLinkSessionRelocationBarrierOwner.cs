using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Service;

internal sealed class ZLinkSessionRelocationBarrierOwner(ZLinkFrameworkRuntime runtime)
    : ISessionRelocationBarrierTarget
{
    public ValueTask<ZLinkServiceWireCodec.SessionRelocationSealedRecord> SealAsync(
        ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken
    )
    {
        if (seal.Coordinator.NodeRid != authenticatedSourceNodeRid)
            throw new InvalidDataException(
                $"Command {(byte)ServiceWireConstants.Command.SessionRelocationSeal} coordinator does not match the authenticated source."
            );
        return runtime.SealCanonicalSessionActorRouteAsync(seal, cancellationToken);
    }

    public ValueTask RouteAsync(
        ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
        ZLinkSessionRelocationAuthenticatedRoute authenticatedRoute,
        CancellationToken cancellationToken
    ) => runtime.RouteCanonicalSessionActorAsync(route, authenticatedRoute, cancellationToken);
}
