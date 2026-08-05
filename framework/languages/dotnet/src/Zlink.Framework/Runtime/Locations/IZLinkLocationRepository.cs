namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Provides every descriptor, owner lease, and object authority operation in
/// one transaction domain. A provider implements one complete location store;
/// callers do not assemble partial capabilities. This is a provider SPI;
/// application code uses the framework object and operational query APIs.
/// </summary>
internal interface IZLinkLocationRepository
{
    ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
        string meshName,
        ZLinkPageRequest page,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationWriteResult> UpdateClientServerAsync(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationWriteStatus> RemoveClientServerAsync(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> ListClientServersAsync(
        string channelName,
        ZLinkPageRequest page,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationWriteResult> UpdateFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationWriteStatus> RemoveFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> ListFanoutPublishersAsync(
        string channelName,
        ZLinkPageRequest page,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
        string ownerId,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
        string ownerId,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
        ZLinkAuthorityKey key,
        string expectedStoreVersion,
        ZLinkAuthorityMutation mutation,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
        string prefix,
        ZLinkAuthorityScanCursor? cursor,
        int limit,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkObjectReserveResult> ReserveAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkObjectCommitResult> CommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
        ZLinkObjectReservation reservation,
        ZLinkObjectCreationCompletion completion,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
        ZLinkCreationOperationId operation,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkObjectAbortResult> AbortAsync(
        ZLinkObjectReservation reservation,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkRelocationCapacityReserveResult> ReserveRelocationCapacityAsync(
        ZLinkRelocationCapacityReservationRequest request,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkRelocationCapacityAbortResult> AbortRelocationCapacityAsync(
        ZLinkRelocationCapacityFence fence,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
        ZLinkAggregatePrepareRequest request,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkAggregateCommitResult> CommitAggregateForRecoveryAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default) =>
        CommitAggregateAsync(fence, cancellationToken);

    ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Removes ephemeral descriptors owned by the exact host lease token.
    /// Durable object authority and reservations remain until an explicit
    /// versioned authority operation removes or completes them.
    /// </summary>
    ValueTask<long> RemoveAllByOwnerAsync(
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Returns an optional counter that changes whenever a MeshNode descriptor
    /// in the named mesh changes. Providers that do not maintain this counter
    /// return null; polling remains the correctness path.
    /// </summary>
    ValueTask<ulong?> GetMeshNodeChangeStampAsync(
        string meshName,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult<ulong?>(null);
}
