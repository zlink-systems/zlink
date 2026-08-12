using Zlink.Framework.Contracts.Locations;

namespace Zlink.Framework.UnitTests;

internal abstract class ZLinkLocationStoreTestDouble : IZLinkLocationRepository
{
    public virtual ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationWriteResult>();

    public virtual ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationWriteStatus>();

    public virtual ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
        ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>();

    public virtual ValueTask<ZLinkLocationWriteResult> UpdateClientServerAsync(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationWriteResult>();

    public virtual ValueTask<ZLinkLocationWriteStatus> RemoveClientServerAsync(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationWriteStatus>();

    public virtual ValueTask<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        ListClientServersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>();

    public virtual ValueTask<ZLinkLocationWriteResult> UpdateFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationWriteResult>();

    public virtual ValueTask<ZLinkLocationWriteStatus> RemoveFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationWriteStatus>();

    public virtual ValueTask<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        ListFanoutPublishersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>();

    public virtual ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
        string ownerId,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkOwnerLeaseClaimResult>();

    public virtual ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
        string ownerId,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkOwnerLeaseReadResult>();

    public virtual ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkOwnerLeaseRenewResult>();

    public virtual ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkOwnerLeaseReleaseResult>();

    public virtual ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkAuthorityReadResult>();

    public virtual ValueTask<ZLinkAuthorityCompareExchangeResult>
        CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkAuthorityCompareExchangeResult>();

    public virtual ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
        string prefix,
        ZLinkAuthorityScanCursor? cursor,
        int limit,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkAuthorityScanResult>();

    public virtual ValueTask<ZLinkObjectReserveResult> ReserveAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkObjectReserveResult>();

    public virtual ValueTask<ZLinkObjectCommitResult> CommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkObjectCommitResult>();

    public virtual ValueTask<ZLinkObjectCreationCompleteResult>
        CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkObjectCreationCompleteResult>();

    public virtual ValueTask<ZLinkCreationTerminalReadResult>
        ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkCreationTerminalReadResult>();

    public virtual ValueTask<ZLinkObjectAbortResult> AbortAsync(
        ZLinkObjectReservation reservation,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkObjectAbortResult>();

    public virtual ValueTask<ZLinkRelocationCapacityReserveResult>
        ReserveRelocationCapacityAsync(
            ZLinkRelocationCapacityReservationRequest request,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkRelocationCapacityReserveResult>();

    public virtual ValueTask<ZLinkRelocationCapacityAbortResult>
        AbortRelocationCapacityAsync(
            ZLinkRelocationCapacityFence fence,
            CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkRelocationCapacityAbortResult>();

    public virtual ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
        ZLinkAggregatePrepareRequest request,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkAggregatePrepareResult>();

    public virtual ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkAggregateCommitResult>();

    public virtual ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default) =>
        Unsupported<ZLinkAggregateAbortResult>();

    public virtual ValueTask<long> RemoveAllByOwnerAsync(
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default) =>
        Unsupported<long>();

    public virtual ValueTask<ulong?> GetMeshNodeChangeStampAsync(
        string meshName,
        CancellationToken cancellationToken = default) =>
        Unsupported<ulong?>();

    private static ValueTask<T> Unsupported<T>() =>
        ValueTask.FromException<T>(new NotSupportedException(
            "This location-store operation is outside the test double's scope."));
}
