namespace Zlink.Framework.Contracts.Locations;

internal readonly record struct ZLinkAuthorityKey(string Value);

internal enum ZLinkPlacementAllocationState
{
    Reserved = 1,
    Active = 2
}

internal sealed record ZLinkPlacementAllocation(
    ZLinkPlacementAllocationState State,
    ZLinkPlacementObjectKind ObjectKind,
    string StableType,
    ZLinkMeshNodeDescriptorKey Descriptor,
    ulong DescriptorLifecycleGeneration,
    ZLinkCapacityVector Capacity);

internal sealed record ZLinkReservedObjectCreation(
    string ReservationId,
    string RequestContentReference,
    ReadOnlyMemory<byte> RequestSha256,
    int RequestEncodedSize);

internal sealed record ZLinkAuthoritySnapshot(
    string StoreVersion,
    ReadOnlyMemory<byte> Payload,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string OwnerId,
    long OwnerLeaseGeneration,
    ZLinkPlacementAllocation Allocation,
    ZLinkReservedObjectCreation? ReservedCreation,
    DateTimeOffset StoreNow);

internal abstract record ZLinkAuthorityReadResult
{
    private protected ZLinkAuthorityReadResult()
    {
    }

    public sealed record Missing(DateTimeOffset StoreNow) : ZLinkAuthorityReadResult;

    public sealed record Found(ZLinkAuthoritySnapshot Snapshot) : ZLinkAuthorityReadResult;
}

internal sealed record ZLinkAuthorityEntry(
    ZLinkAuthorityKey Key,
    ZLinkAuthoritySnapshot Snapshot);

internal readonly record struct ZLinkAuthorityScanCursor
{
    public ZLinkAuthorityScanCursor(string encoded)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(encoded);
        var size = System.Text.Encoding.UTF8.GetByteCount(encoded);
        if (size is < 1 or > 4096)
            throw new ArgumentOutOfRangeException(
                nameof(encoded),
                "Authority scan cursors must be 1 to 4096 UTF-8 bytes.");
        Encoded = encoded;
    }

    public string Encoded { get; }
}

internal sealed record ZLinkAuthorityPage(
    IReadOnlyList<ZLinkAuthorityEntry> Items,
    ZLinkAuthorityScanCursor? NextCursor);

internal abstract record ZLinkAuthorityScanResult
{
    private protected ZLinkAuthorityScanResult()
    {
    }

    public sealed record Page(ZLinkAuthorityPage Value) : ZLinkAuthorityScanResult;

    public sealed record ScanExpired : ZLinkAuthorityScanResult;
}

internal abstract record ZLinkAuthorityMutation
{
    private protected ZLinkAuthorityMutation()
    {
    }

    public sealed record Put(
        ReadOnlyMemory<byte> Payload,
        ZLinkAuthorityGenerationTransition GenerationTransition,
        ZLinkLocationOwnerToken? TargetOwner,
        ZLinkRelocationCapacityFence? RelocationCapacityFence)
        : ZLinkAuthorityMutation;

    /// <summary>
    /// Replaces only the opaque payload after matching the exact store version
    /// and owner token. The current owner lease need not still be live.
    /// </summary>
    public sealed record Restore(
        ReadOnlyMemory<byte> Payload,
        ZLinkLocationOwnerToken ExpectedOwner)
        : ZLinkAuthorityMutation;

    public sealed record Delete : ZLinkAuthorityMutation;
}

internal enum ZLinkAuthorityGenerationTransition
{
    Preserve = 1,
    NewOwner = 2
}

internal readonly record struct ZLinkRelocationCapacityFence(string Value);

internal abstract record ZLinkAuthorityCompareExchangeResult
{
    private protected ZLinkAuthorityCompareExchangeResult()
    {
    }

    public sealed record Stored(ZLinkAuthoritySnapshot Snapshot)
        : ZLinkAuthorityCompareExchangeResult;

    public sealed record Deleted(string StoreVersion, DateTimeOffset StoreNow)
        : ZLinkAuthorityCompareExchangeResult;

    public sealed record Conflict(ZLinkAuthorityReadResult Current)
        : ZLinkAuthorityCompareExchangeResult;

    public sealed record GenerationExhausted : ZLinkAuthorityCompareExchangeResult;
}

internal sealed record ZLinkObjectReservationRequest(
    ZLinkPlacementObjectKind ObjectKind,
    ZLinkAuthorityKey Key,
    string StableType,
    string CreationIntentReference,
    ReadOnlyMemory<byte> CreationIntentHash,
    int CreationIntentEncodedSize,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetNodeLifecycleGeneration,
    ZLinkLocationOwnerToken TargetOwner,
    ReadOnlyMemory<byte> CreatingPayload,
    ZLinkCapacityVector Capacity);

internal sealed record ZLinkObjectReservation(
    ZLinkAuthorityKey Key,
    string StoreVersion,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string ReservationVersion,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetNodeLifecycleGeneration,
    ZLinkLocationOwnerToken TargetOwner);

internal readonly record struct ZLinkCreationOperationId(
    RoutingId SourceNodeRid,
    ulong SourceNodeGeneration,
    ulong OperationIdHigh,
    ulong OperationIdLow);

internal enum ZLinkCreationTerminalState
{
    Created = 1,
    Rejected = 2,
    Failed = 3
}

internal sealed record ZLinkCreationTerminalPublication(
    ZLinkCreationOperationId Operation,
    ReadOnlyMemory<byte> TerminalEnvelope,
    ReadOnlyMemory<byte> TerminalEnvelopeSha256,
    DateTimeOffset ExpiresAt);

internal sealed record ZLinkCreationTerminalRecord(
    ZLinkCreationOperationId Operation,
    string ReservationId,
    ZLinkPlacementObjectKind ObjectKind,
    ZLinkCreationTerminalState State,
    ReadOnlyMemory<byte> TerminalEnvelope,
    ReadOnlyMemory<byte> TerminalEnvelopeSha256,
    DateTimeOffset ExpiresAt,
    DateTimeOffset StoreNow);

internal abstract record ZLinkCreationTerminalReadResult
{
    private protected ZLinkCreationTerminalReadResult()
    {
    }

    public sealed record Missing(DateTimeOffset StoreNow) : ZLinkCreationTerminalReadResult;

    public sealed record Found(ZLinkCreationTerminalRecord Record)
        : ZLinkCreationTerminalReadResult;
}

internal abstract record ZLinkObjectReserveResult
{
    private protected ZLinkObjectReserveResult()
    {
    }

    public sealed record Reserved(ZLinkObjectReservation Reservation)
        : ZLinkObjectReserveResult;

    public sealed record Conflict(ZLinkAuthorityReadResult Current)
        : ZLinkObjectReserveResult;

    public sealed record AlreadyExists(ZLinkAuthoritySnapshot Current)
        : ZLinkObjectReserveResult;

    public sealed record TypeMismatch(ZLinkAuthoritySnapshot Current)
        : ZLinkObjectReserveResult;

    public sealed record PlacementCapacityExhausted : ZLinkObjectReserveResult;

    public sealed record GenerationExhausted : ZLinkObjectReserveResult;
}

internal abstract record ZLinkObjectCommitResult
{
    private protected ZLinkObjectCommitResult()
    {
    }

    public sealed record Committed(ZLinkAuthoritySnapshot Snapshot)
        : ZLinkObjectCommitResult;

    public sealed record AlreadyCommitted(ZLinkAuthoritySnapshot Snapshot)
        : ZLinkObjectCommitResult;

    public sealed record Stale : ZLinkObjectCommitResult;

    public sealed record GenerationExhausted : ZLinkObjectCommitResult;
}

internal abstract record ZLinkObjectCreationCompletion
{
    private protected ZLinkObjectCreationCompletion()
    {
    }

    public sealed record Created(
        ReadOnlyMemory<byte> ReadyPayload,
        ZLinkCreationTerminalPublication Terminal)
        : ZLinkObjectCreationCompletion;

    public sealed record Rejected(ZLinkCreationTerminalPublication Terminal)
        : ZLinkObjectCreationCompletion;

    public sealed record Failed(ZLinkCreationTerminalPublication Terminal)
        : ZLinkObjectCreationCompletion;
}

internal abstract record ZLinkObjectCreationCompleteResult
{
    private protected ZLinkObjectCreationCompleteResult()
    {
    }

    public sealed record Created(
        ZLinkAuthoritySnapshot Snapshot,
        ZLinkCreationTerminalRecord Terminal)
        : ZLinkObjectCreationCompleteResult;

    public sealed record Rejected(ZLinkCreationTerminalRecord Terminal)
        : ZLinkObjectCreationCompleteResult;

    public sealed record Failed(ZLinkCreationTerminalRecord Terminal)
        : ZLinkObjectCreationCompleteResult;

    public sealed record AlreadyCompleted(ZLinkCreationTerminalRecord Terminal)
        : ZLinkObjectCreationCompleteResult;

    public sealed record Stale : ZLinkObjectCreationCompleteResult;

    public sealed record GenerationExhausted : ZLinkObjectCreationCompleteResult;
}

internal abstract record ZLinkObjectAbortResult
{
    private protected ZLinkObjectAbortResult()
    {
    }

    public sealed record Aborted : ZLinkObjectAbortResult;

    public sealed record AlreadyAborted : ZLinkObjectAbortResult;

    public sealed record Stale : ZLinkObjectAbortResult;

    public sealed record GenerationExhausted : ZLinkObjectAbortResult;
}

internal sealed record ZLinkRelocationCapacityReservationRequest(
    Guid ReservationId,
    ZLinkAuthorityKey Key,
    string ExpectedStoreVersion,
    ZLinkPlacementObjectKind ObjectKind,
    string StableType,
    ZLinkMeshNodeDescriptorKey SourceDescriptor,
    ulong SourceNodeLifecycleGeneration,
    ZLinkLocationOwnerToken SourceOwner,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetNodeLifecycleGeneration,
    ZLinkLocationOwnerToken TargetOwner,
    ZLinkCapacityVector Capacity);

internal abstract record ZLinkRelocationCapacityReserveResult
{
    private protected ZLinkRelocationCapacityReserveResult() { }

    public sealed record Reserved(ZLinkRelocationCapacityFence Fence)
        : ZLinkRelocationCapacityReserveResult
    {
        internal ulong TargetAuthorityOwnerGeneration { get; init; }
    }

    public sealed record AlreadyReserved(ZLinkRelocationCapacityFence Fence)
        : ZLinkRelocationCapacityReserveResult
    {
        internal ulong TargetAuthorityOwnerGeneration { get; init; }
    }

    public sealed record Conflict(ZLinkAuthorityReadResult Current)
        : ZLinkRelocationCapacityReserveResult;

    public sealed record TargetUnavailable
        : ZLinkRelocationCapacityReserveResult;

    public sealed record PlacementCapacityExhausted
        : ZLinkRelocationCapacityReserveResult;
}

internal enum ZLinkRelocationCapacityAbortResult
{
    Aborted = 1,
    AlreadyAborted = 2,
    AlreadyCommitted = 3,
    Stale = 4
}

internal sealed record ZLinkAggregateParticipant(
    ZLinkAuthorityKey Key,
    string ExpectedStoreVersion,
    ZLinkAuthorityGenerationTransition OwnerTransition,
    ReadOnlyMemory<byte> AuthorityPayload,
    ReadOnlyMemory<byte> MembershipMutation);

internal sealed record ZLinkAggregatePrepareRequest(
    Guid AggregateId,
    ulong AggregateGeneration,
    IReadOnlyList<ZLinkAggregateParticipant> Participants,
    ReadOnlyMemory<byte> InventoryDigest,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetDescriptorLifecycleGeneration,
    ZLinkCapacityVector Capacity,
    ZLinkLocationOwnerToken TargetOwner,
    bool AllowPreparingTarget = false);

internal readonly record struct ZLinkAggregateFence(
    Guid AggregateId,
    ulong AggregateGeneration);

internal abstract record ZLinkAggregatePrepareResult
{
    private protected ZLinkAggregatePrepareResult()
    {
    }

    public sealed record Prepared(ZLinkAggregateFence Fence)
        : ZLinkAggregatePrepareResult
    {
        internal IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
            TargetAuthorityOwnerGenerations { get; init; } =
                new Dictionary<ZLinkAuthorityKey, ulong>();
    }

    public sealed record AlreadyPrepared(ZLinkAggregateFence Fence)
        : ZLinkAggregatePrepareResult
    {
        internal IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
            TargetAuthorityOwnerGenerations { get; init; } =
                new Dictionary<ZLinkAuthorityKey, ulong>();
    }

    public sealed record Conflict : ZLinkAggregatePrepareResult;

    public sealed record Stale : ZLinkAggregatePrepareResult;

    public sealed record GenerationExhausted : ZLinkAggregatePrepareResult;
}

internal enum ZLinkAggregateCommitResult
{
    Committed = 1,
    AlreadyCommitted = 2,
    Stale = 3,
    GenerationExhausted = 4
}

internal enum ZLinkAggregateAbortResult
{
    Aborted = 1,
    AlreadyAborted = 2,
    Stale = 3
}

internal sealed record ZLinkRelocationStored(
    string Reference,
    uint ChecksumCrc32c,
    DateTimeOffset ExpiresAt,
    DateTimeOffset StoreNow);

internal abstract record ZLinkRelocationReadResult
{
    private protected ZLinkRelocationReadResult()
    {
    }

    public sealed record Found(ReadOnlyMemory<byte> Payload) : ZLinkRelocationReadResult;

    public sealed record Missing : ZLinkRelocationReadResult;
}

internal enum ZLinkRelocationDeleteResult
{
    Deleted = 0,
    Missing = 1
}

internal abstract record ZLinkRelocationRenewResult
{
    private protected ZLinkRelocationRenewResult()
    {
    }

    public sealed record Renewed(DateTimeOffset ExpiresAt, DateTimeOffset StoreNow)
        : ZLinkRelocationRenewResult;

    public sealed record Missing : ZLinkRelocationRenewResult;
}
