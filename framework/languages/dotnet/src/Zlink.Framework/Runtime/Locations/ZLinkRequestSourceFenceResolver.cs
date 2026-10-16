using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkRequestSourceFenceResolver
{
    internal static async ValueTask<ZLinkServiceWireCodec.RequestSourceFence> ResolveAsync(
        IZLinkBackendSpotNode node,
        IZLinkLocationRepository store,
        string meshName,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        CancellationToken cancellationToken
    )
    {
        var sources = node as IZLinkBackendRequestSourceFenceObserver;
        if (sources?.ResolveRequestSourceFence(sourceNodeRid, sourceNodeGeneration) is { } cached)
            return cached;

        var descriptors = await store
            .ListAllMeshNodesAsync(meshName, cancellationToken)
            .ConfigureAwait(false);
        var descriptor = descriptors.SingleOrDefault(value =>
            value.Rid == sourceNodeRid && value.LifecycleGeneration == sourceNodeGeneration
        );
        if (
            descriptor is null
            || sourceNodeGeneration == 0
            || string.IsNullOrWhiteSpace(descriptor.OwnerId)
            || descriptor.LeaseGeneration <= 0
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "The ingress request-source fence is unavailable."
            );
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            descriptor.OwnerId,
            checked((ulong)descriptor.LeaseGeneration),
            sourceNodeRid,
            sourceNodeGeneration
        );
        sources?.ObserveRequestSourceFence(source);
        return source;
    }
}
