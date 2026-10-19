using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Chooses the descriptor that supplies the source request owner values when an owner first
/// accepts a request (Location runtime §9.3, Session·Actor binding §4): the admitted sender's
/// descriptor with the exact RID and node lifecycle generation whose owner lease passes the
/// Location runtime §4.1 check. A polled descriptor is checked against the cached owner lease; a
/// sender missing from the polled descriptors is read from the Store once.
/// </summary>
internal sealed class ZLinkRequestSourceFenceResolver(
    IZLinkMeshNodeLocationResolver peers,
    ZLinkOwnerLeaseTracker leases,
    string meshName
) : IZLinkBackendRequestSourceAcceptance
{
    public async ValueTask<ZLinkServiceWireCodec.RequestSourceFence?> AcceptAsync(
        ZLinkServiceWireCodec.RequestSourceFence? polled,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        CancellationToken cancellationToken
    )
    {
        if (sourceNodeRid.IsEmpty || sourceNodeGeneration == 0)
            return null;
        try
        {
            if (polled is { } known)
                return
                    known.LeaseGeneration <= long.MaxValue
                    && await leases
                        .IsOwnerTokenLiveAsync(
                            new ZLinkLocationOwnerToken(known.OwnerId, (long)known.LeaseGeneration),
                            cancellationToken
                        )
                        .ConfigureAwait(false)
                    ? known
                    : null;

            var descriptors = await peers
                .ListLiveMeshNodesAsync(meshName, cancellationToken)
                .ConfigureAwait(false);
            foreach (var descriptor in descriptors)
                if (
                    descriptor.Rid == sourceNodeRid
                    && descriptor.LifecycleGeneration == sourceNodeGeneration
                    && !string.IsNullOrWhiteSpace(descriptor.OwnerId)
                    && descriptor.LeaseGeneration > 0
                )
                    return new ZLinkServiceWireCodec.RequestSourceFence(
                        descriptor.OwnerId,
                        (ulong)descriptor.LeaseGeneration,
                        sourceNodeRid,
                        sourceNodeGeneration
                    );
            return null;
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            // A Store read that cannot show a live descriptor leaves the request without
            // source owner values; the accepting owner ends it Unavailable (binding §4).
            return null;
        }
    }

    /// <summary>Accepts a request on <paramref name="node"/> or ends it Unavailable.</summary>
    internal static async ValueTask<ZLinkServiceWireCodec.RequestSourceFence> AcceptOrUnavailableAsync(
        IZLinkBackendSpotNode node,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        CancellationToken cancellationToken
    ) =>
        node is IZLinkBackendRequestSourceFenceObserver sources
        && await sources
            .AcceptRequestSourceAsync(sourceNodeRid, sourceNodeGeneration, cancellationToken)
            .ConfigureAwait(false)
            is { } source
            ? source
            : throw Unavailable(sourceNodeRid);

    /// <summary>
    /// With a Location Store, a request the dispatch pump accepted without source owner values
    /// is not accepted: it is not captured or dispatched and ends Unavailable.
    /// </summary>
    internal static bool LacksRecordedSource(
        ZLinkFrameworkRuntime runtime,
        ZLinkServiceWireCodec.RequestSourceFence? recorded
    ) => recorded is null && runtime.Registration.Locations.ResolveStore() is not null;

    internal static ZLinkFrameworkException Unavailable(RoutingId? sourceNodeRid) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Request source '{sourceNodeRid}' has no live MeshNode descriptor.",
            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff
        );
}
