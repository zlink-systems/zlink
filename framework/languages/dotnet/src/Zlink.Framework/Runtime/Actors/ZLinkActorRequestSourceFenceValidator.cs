using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Validates the durable origin of accepted requests before a relocation root
/// makes those requests visible to another owner.
/// </summary>
internal static class ZLinkActorRequestSourceFenceValidator
{
    private const int MaximumConcurrentLeaseReads = 8;

    internal static ValueTask ValidateAsync(
        IZLinkLocationRepository store,
        string meshName,
        MeshNodeStatus localNode,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<ZLinkActorAcceptedRecord> accepted,
        TimeSpan timeout,
        CancellationToken cancellationToken) =>
        ValidateAsync(
            meshName,
            localNode,
            peers,
            accepted,
            timeout,
            store.ReadOwnerLeaseAsync,
            token => store.ListAllMeshNodesAsync(meshName, token),
            cancellationToken);

    internal static async ValueTask ValidateAsync(
        string meshName,
        MeshNodeStatus localNode,
        IReadOnlyList<MeshNodePeer> peers,
        IReadOnlyList<ZLinkActorAcceptedRecord> accepted,
        TimeSpan timeout,
        Func<string, CancellationToken,
            ValueTask<ZLinkOwnerLeaseReadResult>> readOwnerLease,
        Func<CancellationToken,
            ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>>> listMeshNodes,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        ArgumentNullException.ThrowIfNull(localNode);
        ArgumentNullException.ThrowIfNull(peers);
        ArgumentNullException.ThrowIfNull(accepted);
        ArgumentNullException.ThrowIfNull(readOwnerLease);
        ArgumentNullException.ThrowIfNull(listMeshNodes);
        if (accepted.Count == 0) return;

        var fences = accepted
            .Select(static record => record.RequestSource)
            .Distinct()
            .ToArray();
        var ownerIds = fences
            .Select(static fence => fence.OwnerId)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        var leases = new ConcurrentDictionary<string,
            ZLinkOwnerLeaseReadResult>(StringComparer.Ordinal);

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        if (timeout != Timeout.InfiniteTimeSpan)
        {
            if (timeout <= TimeSpan.Zero)
                throw DeadlineExceeded();
            deadline.CancelAfter(timeout);
        }

        try
        {
            var descriptorsTask = listMeshNodes(deadline.Token).AsTask();
            var leasesTask = ReadLeasesAsync(
                ownerIds,
                leases,
                readOwnerLease,
                deadline.Token);
            await Task.WhenAll(descriptorsTask, leasesTask)
                .ConfigureAwait(false);
            var descriptors = await descriptorsTask.ConfigureAwait(false);

            foreach (var fence in fences)
            {
                ValidateLease(fence, leases[fence.OwnerId]);
                ValidateDescriptor(meshName, fence, descriptors);
                ValidateAdmittedIdentity(fence, localNode, peers);
            }
        }
        catch (OperationCanceledException)
            when (!cancellationToken.IsCancellationRequested)
        {
            throw DeadlineExceeded();
        }
    }

    private static Task ReadLeasesAsync(
        IReadOnlyList<string> ownerIds,
        ConcurrentDictionary<string, ZLinkOwnerLeaseReadResult> leases,
        Func<string, CancellationToken,
            ValueTask<ZLinkOwnerLeaseReadResult>> readOwnerLease,
        CancellationToken cancellationToken) =>
        Parallel.ForEachAsync(
            ownerIds,
            new ParallelOptions
            {
                CancellationToken = cancellationToken,
                MaxDegreeOfParallelism = MaximumConcurrentLeaseReads
            },
            async (ownerId, token) =>
            {
                leases[ownerId] = await readOwnerLease(ownerId, token)
                    .ConfigureAwait(false);
            });

    private static void ValidateLease(
        ZLinkServiceWireCodec.RequestSourceFence fence,
        ZLinkOwnerLeaseReadResult lease)
    {
        if (fence.LeaseGeneration > long.MaxValue
            || lease is not ZLinkOwnerLeaseReadResult.Found found
            || !StringComparer.Ordinal.Equals(
                found.Token.OwnerId,
                fence.OwnerId)
            || found.Token.LeaseGeneration
            != checked((long)fence.LeaseGeneration)
            || found.LeaseExpiresAt <= found.StoreNow)
            throw Stale(fence);
    }

    private static void ValidateDescriptor(
        string meshName,
        ZLinkServiceWireCodec.RequestSourceFence fence,
        IReadOnlyList<ZLinkMeshNodeDescriptor> descriptors)
    {
        if (fence.LeaseGeneration > long.MaxValue
            || !descriptors.Any(descriptor =>
                StringComparer.Ordinal.Equals(descriptor.MeshName, meshName)
                && descriptor.Rid == fence.NodeRid
                && descriptor.LifecycleGeneration == fence.NodeGeneration
                && StringComparer.Ordinal.Equals(
                    descriptor.OwnerId,
                    fence.OwnerId)
                && descriptor.LeaseGeneration
                == checked((long)fence.LeaseGeneration)))
            throw Stale(fence);
    }

    private static void ValidateAdmittedIdentity(
        ZLinkServiceWireCodec.RequestSourceFence fence,
        MeshNodeStatus localNode,
        IReadOnlyList<MeshNodePeer> peers)
    {
        var isCurrent = fence.NodeRid == localNode.RoutingId
            ? fence.NodeGeneration == localNode.LifecycleGeneration
            : peers.Any(peer =>
                peer.State == MeshPeerState.Admitted
                && peer.RoutingId == fence.NodeRid
                && peer.LifecycleGeneration == fence.NodeGeneration);
        if (!isCurrent) throw Stale(fence);
    }

    private static ZLinkFrameworkException Stale(
        ZLinkServiceWireCodec.RequestSourceFence fence) => new(
        ZLinkFrameworkErrorKind.Unavailable,
        $"Accepted request source '{fence.NodeRid}' is no longer current.",
        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);

    private static ZLinkFrameworkException DeadlineExceeded() => new(
        ZLinkFrameworkErrorKind.DeadlineExceeded,
        "Accepted request source validation exceeded the relocation deadline.",
        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
}
