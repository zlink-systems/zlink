using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Internal;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Spots;
using AggregateInventoryLeafEntry = Zlink.Framework.Runtime.Locations.ZLinkAggregateInventory.Entry;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private const string AggregatePrefix = Prefix + "aggregate:";
    private static readonly TimeSpan AmbiguousReconciliationTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan WriteConflictRetryWindow = TimeSpan.FromSeconds(5);
    private const int WriteConflictRetryLimit = 64;
    private static readonly TimeSpan AggregateStagingRetention = TimeSpan.FromHours(24);
    private const int MaximumStoreConcurrency = 64;
    private readonly SemaphoreSlim aggregateRecoveryGate = new(1, 1);
    private int aggregateStagingRecoveryCompleted;

    private async ValueTask<ZLinkFrameworkErrorKind?> AddMembershipCommitConditionsAsync(
        ReadOnlyMemory<byte> payload,
        ZLinkAuthoritySnapshot current,
        List<ZLinkStoreCondition> conditions,
        CancellationToken cancellationToken,
        ZLinkAggregatePrepareRequest? aggregate = null
    )
    {
        if (
            current.Allocation.ObjectKind != ZLinkPlacementObjectKind.Actor
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(payload.Span, out var actor)
            || actor.CurrentSpotKind != ZLinkSpotKind.User
        )
            return null;
        if (
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                current.Payload.Span,
                out var previous
            )
            && previous.State == ZLinkActorAuthorityState.Ready
            && previous.CurrentSpotKind == ZLinkSpotKind.User
            && previous.CurrentSpotId == actor.CurrentSpotId
            && previous.CurrentSpotGeneration == actor.CurrentSpotGeneration
        )
            return null;
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(actor.CurrentSpotId);
        var spot = await ReadAuthorityRecordAsync(
                key,
                knownMeta: null,
                projectCommittedAggregate: false,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (spot is null)
            return ZLinkFrameworkErrorKind.NotFound;
        if (
            spot.Snapshot.ObjectGeneration != actor.CurrentSpotGeneration
            || spot.Snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.UserSpot
            || spot.Snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                spot.Snapshot.Payload.Span,
                out var target
            )
            || target.State != ZLinkUserSpotAuthorityState.Ready
        )
            return ZLinkFrameworkErrorKind.Unavailable;
        var participant = aggregate?.Participants.SingleOrDefault(p => p.Key == key);
        if (
            spot.Meta.AggregateFence is { } fence
            && (
                aggregate is null
                || fence
                    != new ZLinkAggregateFence(aggregate.AggregateId, aggregate.AggregateGeneration)
            )
        )
        {
            var pending = await ReadRecordAsync<AggregateRecord>(
                    AggregateKey(fence),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (pending?.Record.Status == AggregateStatus.Prepared)
                return ZLinkFrameworkErrorKind.Unavailable;
            if (pending is not null)
                AddCondition(
                    conditions,
                    new ZLinkStoreCondition.Version(AggregateKey(fence), pending.Version)
                );
        }
        var token =
            participant?.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner
                ? aggregate!.TargetOwner
                : new ZLinkLocationOwnerToken(
                    spot.Snapshot.OwnerId,
                    spot.Snapshot.OwnerLeaseGeneration
                );
        var owner = await ReadLiveOwnerAsync(token, cancellationToken).ConfigureAwait(false);
        if (owner is null)
            return ZLinkFrameworkErrorKind.Unavailable;
        AddCondition(
            conditions,
            new ZLinkStoreCondition.Version(AuthorityMetaKey(key), spot.Version)
        );
        AddCondition(
            conditions,
            new ZLinkStoreCondition.Value(OwnerKey(token.OwnerId), owner.Value)
        );
        return null;
    }

    public async ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken = default
    )
    {
        ValidateAuthorityKey(key);
        var stored = await ReadAuthorityRecordAsync(key, cancellationToken).ConfigureAwait(false);
        ZLinkAuthorityReadResult result = stored is null
            ? new ZLinkAuthorityReadResult.Missing(
                await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false)
            )
            : new ZLinkAuthorityReadResult.Found(stored.Snapshot);
        if (AuthorityObservedAsync is { } observed)
            await observed(key, stored?.Snapshot.StoreVersion).ConfigureAwait(false);
        return result;
    }

    public async ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
        ZLinkAuthorityKey key,
        string expectedStoreVersion,
        ZLinkAuthorityMutation mutation,
        CancellationToken cancellationToken = default
    )
    {
        ValidateAuthorityKey(key);
        ArgumentException.ThrowIfNullOrWhiteSpace(expectedStoreVersion);
        ArgumentNullException.ThrowIfNull(mutation);
        ZLinkAuthorityMutation.Validate(mutation);

        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            var metaKey = AuthorityMetaKey(key);
            var current = await ReadAuthorityRecordAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (
                current is null
                || current.Snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
                || current.Version.Value != expectedStoreVersion
                || current.Meta.AggregateFence is not null
            )
            {
                if (
                    mutation is ZLinkAuthorityMutation.Delete
                    && current is not null
                    && current.Version.Value != expectedStoreVersion
                )
                {
                    // A source handoff cleanup can arrive after the target has
                    // published a newer authority version. Submit the old
                    // fence to the opaque provider so an in-flight cleanup
                    // remains observable and retryable. The expected version
                    // cannot match the target row, so this batch cannot delete
                    // the target's single authority row.
                    await _provider
                        .WriteAsync(
                            new ZLinkStoreWriteRequest(
                                [
                                    new ZLinkStoreCondition.Version(
                                        metaKey,
                                        new ZLinkStoreVersion(expectedStoreVersion)
                                    ),
                                ],
                                [new ZLinkStoreMutation.Delete(metaKey)]
                            ),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }

                //  Four separate reasons collapse into one Conflict, and the caller
                //  reports all of them as "authority changed".
                if (Diagnostics.ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
                    Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"cas_conflict_reason missing={current is null} "
                            + $"state={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(current?.Snapshot.Allocation.State)} "
                            + $"version_match={current is not null && current.Version.Value == expectedStoreVersion} "
                            + $"fence={Diagnostics.ZLinkFrameworkDebugLog.OrAbsent(current?.Meta.AggregateFence)}"
                    );
                return Conflict(current);
            }

            var conditions = new List<ZLinkStoreCondition>
            {
                new ZLinkStoreCondition.Version(metaKey, current.Version),
            };
            var mutations = new List<ZLinkStoreMutation>();
            AuthorityMeta? meta = null;
            if (mutation is ZLinkAuthorityMutation.Restore restore)
            {
                ValidateAuthorityPayload(restore.Payload);
                if (
                    current.Snapshot.OwnerId != restore.ExpectedOwner.OwnerId
                    || current.Snapshot.OwnerLeaseGeneration
                        != restore.ExpectedOwner.LeaseGeneration
                )
                    return Conflict(current);
                if (
                    (
                        await AddMembershipCommitConditionsAsync(
                                restore.Payload,
                                current.Snapshot,
                                conditions,
                                cancellationToken
                            )
                            .ConfigureAwait(false)
                    ) is
                    { } membershipFailure
                )
                    return new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(current.Snapshot)
                    )
                    {
                        MembershipFailure = membershipFailure,
                    };
                meta = current.Meta with
                {
                    Payload = restore.Payload.ToArray(),
                    Aggregate = ApplyAuthorityRelocation(current, restore),
                };
            }
            else if (mutation is ZLinkAuthorityMutation.Delete)
            {
                var owner = await ReadLiveOwnerAsync(
                        new ZLinkLocationOwnerToken(
                            current.Snapshot.OwnerId,
                            current.Snapshot.OwnerLeaseGeneration
                        ),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (owner is null)
                    return Conflict(current);
                await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
                conditions.Add(
                    new ZLinkStoreCondition.Value(OwnerKey(owner.Token.OwnerId), owner.Value)
                );
                mutations.Add(new ZLinkStoreMutation.Delete(metaKey));
                AddCapacityMutations(mutations, current.Snapshot.Allocation, active: -1);
            }
            else
            {
                var put = (ZLinkAuthorityMutation.Put)mutation;
                ValidateAuthorityPayload(put.Payload);
                if (
                    (
                        await AddMembershipCommitConditionsAsync(
                                put.Payload,
                                current.Snapshot,
                                conditions,
                                cancellationToken
                            )
                            .ConfigureAwait(false)
                    ) is
                    { } membershipFailure
                )
                    return new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(current.Snapshot)
                    )
                    {
                        MembershipFailure = membershipFailure,
                    };
                if (!ZLinkAuthorityMutation.MatchesPreservedOwner(current.Snapshot, put))
                    return Conflict(current);
                var changesOwner =
                    put.GenerationTransition == ZLinkAuthorityGenerationTransition.NewOwner;
                var targetOwner =
                    put.TargetOwner
                    ?? new ZLinkLocationOwnerToken(
                        current.Snapshot.OwnerId,
                        current.Snapshot.OwnerLeaseGeneration
                    );
                if (
                    put.GenerationTransition == ZLinkAuthorityGenerationTransition.Preserve
                    && put.TargetOwner is not null
                )
                {
                    var descriptorKey = MeshKey(
                        current.Snapshot.Allocation.Descriptor.MeshName,
                        current.Snapshot.Allocation.Descriptor.Rid
                    );
                    var descriptor = await _provider
                        .ReadAsync(descriptorKey, cancellationToken)
                        .ConfigureAwait(false);
                    if (
                        descriptor is not ZLinkStoreReadResult.Found foundDescriptor
                        || !ZLinkAuthorityMutation.MatchesPreservedDescriptor(
                            current.Snapshot,
                            DecodeDescriptor<ZLinkMeshNodeDescriptor>(
                                foundDescriptor.Value.Bytes
                            ).Descriptor
                        )
                    )
                        return Conflict(current);
                    AddCondition(
                        conditions,
                        new ZLinkStoreCondition.Version(
                            descriptorKey,
                            foundDescriptor.Value.Version
                        )
                    );
                }
                var liveOwner = await ReadLiveOwnerAsync(targetOwner, cancellationToken)
                    .ConfigureAwait(false);
                if (liveOwner is null)
                    return Conflict(current) with
                    {
                        MembershipFailure = ZLinkFrameworkErrorKind.Unavailable,
                    };

                AddCondition(
                    conditions,
                    new ZLinkStoreCondition.Value(OwnerKey(targetOwner.OwnerId), liveOwner.Value)
                );
                var nextAllocation = current.Snapshot.Allocation;
                if (changesOwner)
                {
                    var targetAllocation = put.TargetAllocation!;
                    if (
                        targetAllocation.ObjectKind != current.Snapshot.Allocation.ObjectKind
                        || !StringComparer.Ordinal.Equals(
                            targetAllocation.StableType,
                            current.Snapshot.Allocation.StableType
                        )
                        || targetAllocation.Capacity != current.Snapshot.Allocation.Capacity
                    )
                        return Conflict(current);
                    var target = await ReadEligibleTargetAsync(
                            targetAllocation.Descriptor,
                            targetAllocation.DescriptorLifecycleGeneration,
                            targetOwner,
                            targetAllocation.ObjectKind,
                            targetAllocation.StableType,
                            cancellationToken,
                            requireNewPlacementEligibility: false
                        )
                        .ConfigureAwait(false);
                    if (target is null)
                        return Conflict(current);
                    AddCondition(conditions, target.DescriptorCondition);
                    AddCondition(conditions, target.OwnerCondition);

                    var deltas = new Dictionary<ZLinkStoreKey, long>();
                    var source = current.Snapshot.Allocation;
                    AccumulateCapacity(deltas, source.Descriptor, source.Capacity, active: -1);
                    AccumulateCapacity(
                        deltas,
                        targetAllocation.Descriptor,
                        targetAllocation.Capacity,
                        active: 1
                    );
                    AppendCapacityMutations(mutations, deltas);
                    nextAllocation = targetAllocation;
                }

                var nextObjectGeneration = current.Meta.ObjectGeneration;
                var nextAuthorityOwnerGeneration = current.Meta.AuthorityOwnerGeneration;
                var reincarnating =
                    put.GenerationTransition == ZLinkAuthorityGenerationTransition.Reincarnate;
                if (reincarnating)
                {
                    var objectCounter = await ReadGenerationCounterAsync(
                            ObjectGenerationCounterKey(key),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    if (objectCounter.Value == MaximumGeneration)
                        return new ZLinkAuthorityCompareExchangeResult.GenerationExhausted();
                    if (objectCounter.Value <= current.Meta.ObjectGeneration)
                        throw new InvalidDataException(
                            "The Location Store object counter precedes its authority."
                        );
                    nextObjectGeneration = objectCounter.Issue(conditions, mutations);
                }
                if (reincarnating || changesOwner)
                {
                    var counter = await ReadGenerationCounterAsync(
                            AuthorityOwnerGenerationCounterKey(key),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    if (counter.Value == MaximumGeneration)
                        return new ZLinkAuthorityCompareExchangeResult.GenerationExhausted();
                    if (
                        counter.Value <= current.Meta.AuthorityOwnerGeneration
                        || (
                            changesOwner
                            && put.TargetAuthorityOwnerGeneration != 0
                            && put.TargetAuthorityOwnerGeneration != counter.Value
                        )
                    )
                        return Conflict(current);
                    nextAuthorityOwnerGeneration = counter.Issue(conditions, mutations);
                }
                meta = current.Meta with
                {
                    Payload = put.Payload.ToArray(),
                    Aggregate = ApplyAuthorityRelocation(current, put),
                    ObjectGeneration = nextObjectGeneration,
                    AuthorityOwnerGeneration = nextAuthorityOwnerGeneration,
                    OwnerId = targetOwner.OwnerId,
                    OwnerLeaseGeneration = targetOwner.LeaseGeneration,
                    Allocation = nextAllocation,
                };
            }
            if (meta is not null)
                mutations.Add(new ZLinkStoreMutation.Put(metaKey, Encode(meta), null));
            var result = await _provider
                .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), cancellationToken)
                .ConfigureAwait(false);
            if (result is not ZLinkStoreWriteResult.Applied applied)
                continue;
            if (AuthorityObservedAsync is { } observed)
                await observed(key, meta is null ? null : applied.PutVersions[metaKey].Value)
                    .ConfigureAwait(false);
            return meta is null
                ? new ZLinkAuthorityCompareExchangeResult.Deleted(
                    current.Version.Value,
                    applied.StoreNow
                )
                : new ZLinkAuthorityCompareExchangeResult.Stored(
                    Snapshot(meta, applied.PutVersions[metaKey], applied.StoreNow, meta.Payload)
                );
        }
    }

    public async ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
        string prefix,
        ZLinkAuthorityScanCursor? cursor,
        int limit,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentNullException.ThrowIfNull(prefix);
        if (limit is < 1 or > 1000)
            throw new ArgumentOutOfRangeException(nameof(limit));
        var result = await _provider
            .ScanAsync(
                new ZLinkStoreScanRequest(
                    AuthorityMetaPrefix(prefix),
                    cursor is null ? null : new ZLinkStoreScanCursor(cursor.Value.Encoded),
                    limit
                ),
                cancellationToken
            )
            .ConfigureAwait(false);
        if (result is ZLinkStoreScanResult.Expired)
            return new ZLinkAuthorityScanResult.ScanExpired();
        var page = ((ZLinkStoreScanResult.Page)result).Value;
        var entries = new List<ZLinkAuthorityEntry>(page.Items.Count);
        foreach (var item in page.Items)
        {
            var key = DecodeAuthorityKey(item.Key);
            var stored = await ReadAuthorityRecordAsync(
                    key,
                    item.Value,
                    projectCommittedAggregate: true,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (stored is not null)
                entries.Add(new ZLinkAuthorityEntry(key, stored.Snapshot));
        }
        return new ZLinkAuthorityScanResult.Page(
            new ZLinkAuthorityPage(
                entries,
                page.NextCursor is null
                    ? null
                    : new ZLinkAuthorityScanCursor(page.NextCursor.Value.Value)
            )
        );
    }

    public ValueTask<ZLinkObjectReserveResult> ReserveAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken = default
    ) =>
        request.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
        && Diagnostics.ZLinkRuntimeMetrics.InstanceSpotTakeoversEnabled
            ? ReserveWithTakeoverMetricAsync(request, cancellationToken)
            : ReserveCoreAsync(request, cancellationToken);

    private async ValueTask<ZLinkObjectReserveResult> ReserveWithTakeoverMetricAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken
    )
    {
        Diagnostics.ZLinkInstanceSpotTakeoverOutcome? takeoverOutcome = null;
        try
        {
            var result = await ReserveCoreAsync(
                    request,
                    cancellationToken,
                    () => takeoverOutcome = Diagnostics.ZLinkInstanceSpotTakeoverOutcome.Failed
                )
                .ConfigureAwait(false);
            if (takeoverOutcome is not null)
                takeoverOutcome = result switch
                {
                    ZLinkObjectReserveResult.Reserved => Diagnostics
                        .ZLinkInstanceSpotTakeoverOutcome
                        .Claimed,
                    ZLinkObjectReserveResult.Conflict or ZLinkObjectReserveResult.AlreadyExists =>
                        Diagnostics.ZLinkInstanceSpotTakeoverOutcome.Lost,
                    _ => Diagnostics.ZLinkInstanceSpotTakeoverOutcome.Failed,
                };
            return result;
        }
        finally
        {
            if (takeoverOutcome is not null)
                Diagnostics.ZLinkRuntimeMetrics.RecordInstanceSpotTakeover(
                    request.TargetDescriptor.MeshName,
                    request.StableType,
                    takeoverOutcome.Value
                );
        }
    }

    private async ValueTask<ZLinkObjectReserveResult> ReserveCoreAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken,
        Action? noteTakeover = null
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        ValidateReservation(request);
        var reservationId = Guid.NewGuid().ToString("N");
        string? reclaimVersion = null;
        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            var current = await ReadAuthorityRecordAsync(request.Key, cancellationToken)
                .ConfigureAwait(false);
            if (current is not null)
            {
                if (
                    current.Snapshot.Allocation.ObjectKind != request.ObjectKind
                    || current.Snapshot.Allocation.StableType != request.StableType
                )
                    return new ZLinkObjectReserveResult.TypeMismatch(current.Snapshot);
                if (reclaimVersion is not null && current.Snapshot.StoreVersion != reclaimVersion)
                    return new ZLinkObjectReserveResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(current.Snapshot)
                    );
                var reclaim = await TryReclaimStaleAuthorityAsync(
                        current,
                        cancellationToken,
                        new ZLinkCreationAuthorityRecovery(
                            request.TargetDescriptor,
                            request.TargetNodeLifecycleGeneration
                        ),
                        noteTakeover
                    )
                    .ConfigureAwait(false);
                if (reclaim == StaleAuthorityReclaimResult.Reclaimed)
                {
                    reclaimVersion = null;
                    continue;
                }
                if (reclaim == StaleAuthorityReclaimResult.Conflict)
                {
                    reclaimVersion = current.Snapshot.StoreVersion;
                    continue;
                }
                return current.Snapshot.Allocation.State == ZLinkPlacementAllocationState.Active
                    ? new ZLinkObjectReserveResult.AlreadyExists(current.Snapshot)
                    : new ZLinkObjectReserveResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(current.Snapshot)
                    );
            }
            if (reclaimVersion is not null)
                return new ZLinkObjectReserveResult.Conflict(
                    new ZLinkAuthorityReadResult.Missing(
                        await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false)
                    )
                );
            var target = await ReadEligibleTargetAsync(
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.TargetOwner,
                    request.ObjectKind,
                    request.StableType,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (target is null)
                return new ZLinkObjectReserveResult.Conflict(
                    new ZLinkAuthorityReadResult.Missing(
                        await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false)
                    )
                );

            var capacityConditions = CapacityReservationConditions(
                target.Descriptor,
                request.Capacity
            );
            if (capacityConditions is null)
                return new ZLinkObjectReserveResult.PlacementCapacityExhausted();
            var objectCounter = await ReadGenerationCounterAsync(
                    ObjectGenerationCounterKey(request.Key),
                    cancellationToken
                )
                .ConfigureAwait(false);
            var authorityCounter = await ReadGenerationCounterAsync(
                    AuthorityOwnerGenerationCounterKey(request.Key),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (
                objectCounter.Value == MaximumGeneration
                || authorityCounter.Value == MaximumGeneration
            )
                return new ZLinkObjectReserveResult.GenerationExhausted();

            var objectGeneration = objectCounter.Value;
            var authorityOwnerGeneration = authorityCounter.Value;
            var allocation = new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Reserved,
                request.ObjectKind,
                request.StableType,
                request.TargetDescriptor,
                request.TargetNodeLifecycleGeneration,
                request.Capacity
            );
            var meta = new AuthorityMeta(
                request.CreatingPayload.ToArray(),
                objectGeneration,
                authorityOwnerGeneration,
                request.TargetOwner.OwnerId,
                request.TargetOwner.LeaseGeneration,
                allocation,
                new ZLinkReservedObjectCreation(
                    reservationId,
                    request.CreationIntentReference,
                    request.CreationIntentHash.ToArray(),
                    request.CreationIntentEncodedSize
                )
            );
            var metaKey = AuthorityMetaKey(request.Key);
            var conditions = new List<ZLinkStoreCondition>
            {
                new ZLinkStoreCondition.Missing(metaKey),
                target.DescriptorCondition,
                target.OwnerCondition,
            };
            conditions.AddRange(capacityConditions);
            var mutations = new List<ZLinkStoreMutation>
            {
                new ZLinkStoreMutation.Put(metaKey, Encode(meta), null),
            };
            objectCounter.Issue(conditions, mutations);
            authorityCounter.Issue(conditions, mutations);
            AddCapacityMutations(mutations, allocation, pending: 1);
            var result = await _provider
                .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Conflict)
            {
                if (
                    capacityConditions.Count != 0
                    && await ReservationAuthorityMatchesAsync(
                            new ZLinkStoreCondition.Missing(metaKey),
                            target,
                            cancellationToken
                        )
                        .ConfigureAwait(false)
                    && await IsReservationCapacityExceededAsync(
                            target.Descriptor,
                            capacityConditions,
                            cancellationToken
                        )
                        .ConfigureAwait(false)
                )
                    return new ZLinkObjectReserveResult.PlacementCapacityExhausted();
                continue;
            }
            var applied = (ZLinkStoreWriteResult.Applied)result;
            return new ZLinkObjectReserveResult.Reserved(
                new ZLinkObjectReservation(
                    request.Key,
                    applied.PutVersions[metaKey].Value,
                    objectGeneration,
                    authorityOwnerGeneration,
                    reservationId,
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.TargetOwner
                )
            );
        }
    }

    public ValueTask<bool> CleanupDiscardedAuthorityAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot discarded,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default
    ) =>
        ZLinkAuthorityCleanup.CleanupDiscardedAsync(
            this,
            async (snapshot, ct) =>
            {
                var current = await ReadAuthorityRecordAsync(key, ct).ConfigureAwait(false);
                if (current is null || current.Snapshot.StoreVersion != snapshot.StoreVersion)
                    return false;
                return await TryReclaimStaleAuthorityAsync(current, ct, discarded: true)
                        .ConfigureAwait(false) == StaleAuthorityReclaimResult.Reclaimed;
            },
            key,
            discarded,
            owner,
            cancellationToken
        );

    private async ValueTask<StaleAuthorityReclaimResult> TryReclaimStaleAuthorityAsync(
        StoredAuthority current,
        CancellationToken cancellationToken,
        ZLinkCreationAuthorityRecovery? creationTarget = null,
        Action? noteTakeover = null,
        ZLinkLocationOwnerToken? requiredOwner = null,
        bool discarded = false
    )
    {
        var active = current.Snapshot.Allocation.State == ZLinkPlacementAllocationState.Active;
        var actorClosing = ZLinkActorAuthorityPayloadCodec.IsClosing(current.Snapshot);
        if (active && creationTarget is null && !discarded && !actorClosing)
            return StaleAuthorityReclaimResult.NotReclaimable;

        // Relocation authority retains its own recovery protocol.
        if (current.Meta.AggregateFence is not null)
            return StaleAuthorityReclaimResult.RecoveryRequired;
        if (
            current.Snapshot.Relocation is not null
            || ZLinkRelocationAuthorityPayloadCodec.TryDecode(current.Snapshot.Payload.Span, out _)
        )
            return StaleAuthorityReclaimResult.RecoveryRequired;
        var descriptorKey = MeshKey(
            current.Snapshot.Allocation.Descriptor.MeshName,
            current.Snapshot.Allocation.Descriptor.Rid
        );
        var owners = new Dictionary<string, ZLinkStoreReadResult>(StringComparer.Ordinal)
        {
            [current.Snapshot.OwnerId] = await _provider
                .ReadAsync(OwnerKey(current.Snapshot.OwnerId), cancellationToken)
                .ConfigureAwait(false),
        };
        var currentOwner = new ZLinkLocationOwnerToken(
            current.Snapshot.OwnerId,
            current.Snapshot.OwnerLeaseGeneration
        );
        var ownerLive = IsOwnerLeaseLive(owners[current.Snapshot.OwnerId], currentOwner);
        var releaseWaitElapsed = true;
        var descriptorRead = await _provider
            .ReadAsync(descriptorKey, cancellationToken)
            .ConfigureAwait(false);
        if (
            requiredOwner is { } owner
            && (
                !ownerLive
                || owner.OwnerId != current.Snapshot.OwnerId
                || owner.LeaseGeneration != current.Snapshot.OwnerLeaseGeneration
            )
        )
            return StaleAuthorityReclaimResult.NotReclaimable;
        if (!ownerLive)
            noteTakeover?.Invoke();
        if (active && ownerLive)
            return StaleAuthorityReclaimResult.OwnerLive;

        var sourceDescriptor = descriptorRead is ZLinkStoreReadResult.Found descriptorFound
            ? DecodeDescriptor<ZLinkMeshNodeDescriptor>(descriptorFound.Value.Bytes).Descriptor
            : null;
        ZLinkStoreCondition.Version? targetDescriptorCondition = null;
        if (active && !discarded && !actorClosing && creationTarget is { } target)
        {
            var targetKey = MeshKey(target.TargetDescriptor.MeshName, target.TargetDescriptor.Rid);
            var targetDescriptor = sourceDescriptor;
            if (targetKey != descriptorKey)
            {
                var targetRead = await _provider
                    .ReadAsync(targetKey, cancellationToken)
                    .ConfigureAwait(false);
                targetDescriptor = null;
                if (targetRead is ZLinkStoreReadResult.Found policyRow)
                {
                    targetDescriptor = DecodeDescriptor<ZLinkMeshNodeDescriptor>(
                        policyRow.Value.Bytes
                    ).Descriptor;
                    targetDescriptorCondition = new ZLinkStoreCondition.Version(
                        targetKey,
                        policyRow.Value.Version
                    );
                }
            }
            ZLinkAuthorityCleanup.RequireDisabledCreationPolicy(
                current.Key,
                current.Snapshot.Allocation,
                target.TargetNodeLifecycleGeneration,
                targetDescriptor
            );
        }
        IReadOnlyList<StoredAuthority> members = [];
        var endedUserSpotId = active
            ? ZLinkAuthorityCleanup.RequireReclaimableAuthority(current.Snapshot)
            : null;
        if (active && !ownerLive)
            _ownerLeaseReleaseWait.Observe(currentOwner);
        if (endedUserSpotId is not null && !discarded)
            members = await ReadEndedUserSpotMembersAsync(
                    endedUserSpotId,
                    current.Snapshot.ObjectGeneration,
                    cancellationToken
                )
                .ConfigureAwait(false);
        if (
            ownerLive
            && sourceDescriptor?.LifecycleGeneration
                == current.Snapshot.Allocation.DescriptorLifecycleGeneration
        )
            return StaleAuthorityReclaimResult.OwnerLive;
        ZLinkStoreCondition descriptorCondition = descriptorRead
            is ZLinkStoreReadResult.Found sourceRow
            ? new ZLinkStoreCondition.Version(descriptorKey, sourceRow.Value.Version)
            : new ZLinkStoreCondition.Missing(descriptorKey);
        if (!active && current.Meta.ReservedCreation is null)
            return StaleAuthorityReclaimResult.RecoveryRequired;

        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(AuthorityMetaKey(current.Key), current.Version),
            descriptorCondition,
        };
        if (targetDescriptorCondition is not null)
            conditions.Add(targetDescriptorCondition);
        var mutations = new List<ZLinkStoreMutation>
        {
            new ZLinkStoreMutation.Delete(AuthorityMetaKey(current.Key)),
        };
        // §6.1: every remaining member must be an ended-owner member. Each one
        // is released in the same batch, fenced by its authority version and
        // by the observed state of its own owner lease.
        foreach (var member in members)
        {
            if (!owners.TryGetValue(member.Snapshot.OwnerId, out var memberOwner))
            {
                memberOwner = await _provider
                    .ReadAsync(OwnerKey(member.Snapshot.OwnerId), cancellationToken)
                    .ConfigureAwait(false);
                owners.Add(member.Snapshot.OwnerId, memberOwner);
            }
            var memberOwnerToken = new ZLinkLocationOwnerToken(
                member.Snapshot.OwnerId,
                member.Snapshot.OwnerLeaseGeneration
            );
            if (IsOwnerLeaseLive(memberOwner, memberOwnerToken))
            {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "User Spot still has a member with a live owner lease.",
                    ZLinkRetryAdvice.RetryAfterStateChange
                );
            }
            _ownerLeaseReleaseWait.Observe(memberOwnerToken);
            conditions.Add(
                new ZLinkStoreCondition.Version(AuthorityMetaKey(member.Key), member.Version)
            );
            mutations.Add(new ZLinkStoreMutation.Delete(AuthorityMetaKey(member.Key)));
        }
        foreach (var (ownerId, read) in owners)
            conditions.Add(
                read is ZLinkStoreReadResult.Found found
                    ? new ZLinkStoreCondition.Version(OwnerKey(ownerId), found.Value.Version)
                    : new ZLinkStoreCondition.Missing(OwnerKey(ownerId))
            );
        if (active && !ownerLive)
        {
            releaseWaitElapsed = _ownerLeaseReleaseWait.HasElapsed(
                currentOwner,
                _ownerLeaseFencingMargin
            );
        }
        foreach (var member in members)
            releaseWaitElapsed &= _ownerLeaseReleaseWait.HasElapsed(
                new ZLinkLocationOwnerToken(
                    member.Snapshot.OwnerId,
                    member.Snapshot.OwnerLeaseGeneration
                ),
                _ownerLeaseFencingMargin
            );
        if (!releaseWaitElapsed)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The Location owner lease release wait has not elapsed.",
                ZLinkRetryAdvice.RetryAfterStateChange
            );
        await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
        var deltas = new Dictionary<ZLinkStoreKey, long>();
        foreach (var member in members)
        {
            var allocation = member.Snapshot.Allocation;
            AccumulateCapacity(deltas, allocation.Descriptor, allocation.Capacity, active: -1);
        }
        var released = current.Snapshot.Allocation;
        AccumulateCapacity(
            deltas,
            released.Descriptor,
            released.Capacity,
            pending: active ? 0 : -1,
            active: active ? -1 : 0
        );
        AppendCapacityMutations(mutations, deltas);
        if (conditions.Count + deltas.Count > ZLinkLocationStoreLimits.MaximumUniqueKeys)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "User Spot member release exceeds one Location Store batch.",
                ZLinkRetryAdvice.RetryAfterStateChange
            );

        var result = await _provider
            .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkStoreWriteResult.Applied)
            return StaleAuthorityReclaimResult.Conflict;
        return StaleAuthorityReclaimResult.Reclaimed;
    }

    private static bool IsOwnerLeaseLive(ZLinkStoreReadResult read, ZLinkLocationOwnerToken token)
    {
        if (read is not ZLinkStoreReadResult.Found found)
            return false;
        var owner = DecodeOwner(found.Value.Bytes);
        if (owner.OwnerId != token.OwnerId || found.Value.ExpiresAt is null)
            throw new InvalidDataException("The Location Store owner lease record is invalid.");
        return owner.LeaseGeneration == token.LeaseGeneration
            && found.Value.ExpiresAt > found.Value.StoreNow;
    }

    // Reads every Actor membership of one User Spot incarnation from one
    // complete snapshot scan. An expired snapshot restarts the scan; a Store
    // failure propagates. A member that is relocating, part of an aggregate or
    // not yet activated keeps the Spot (§6.1).
    private async ValueTask<IReadOnlyList<StoredAuthority>> ReadEndedUserSpotMembersAsync(
        string spotId,
        ulong objectGeneration,
        CancellationToken cancellationToken
    )
    {
        var prefix = AuthorityMetaPrefix(
            ZLinkAuthorityKeyCodec.Prefix(ZLinkAuthorityKeyKind.Actor)
        );
        var members = new List<StoredAuthority>();
        ZLinkStoreScanCursor? cursor = null;
        for (; ; )
        {
            var scan = await _provider
                .ScanAsync(
                    new ZLinkStoreScanRequest(
                        prefix,
                        cursor,
                        ZLinkPageRequestPolicy.MaximumPageSize
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                members.Clear();
                cursor = null;
                continue;
            }
            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            foreach (var item in page.Items)
            {
                var stored = await ReadAuthorityRecordAsync(
                        DecodeAuthorityKey(item.Key),
                        item.Value,
                        projectCommittedAggregate: true,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (
                    stored is null
                    || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                        stored.Snapshot.Payload.Span,
                        out var member
                    )
                    || member.CurrentSpotKind != ZLinkSpotKind.User
                    || member.CurrentSpotId != spotId
                    || member.CurrentSpotGeneration != objectGeneration
                )
                    continue;
                if (
                    stored.Meta.AggregateFence is not null
                    || stored.Snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
                    || stored.Snapshot.Relocation is not null
                    || !ZLinkActorAuthorityPayloadCodec.IsSteadyReady(stored.Snapshot.Payload.Span)
                )
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "User Spot still has a relocating or unactivated member.",
                        ZLinkRetryAdvice.RetryAfterStateChange
                    );
                members.Add(stored);
            }
            cursor = page.NextCursor;
            if (cursor is null)
                return members;
        }
    }

    public ValueTask<bool> ReleaseEndedReservationAsync(
        ZLinkAuthorityKey key,
        string expectedStoreVersion,
        ZLinkCreationAuthorityRecovery? creationTarget = null,
        CancellationToken cancellationToken = default
    ) =>
        ReleaseEndedReservationAsync(
            key,
            expectedStoreVersion,
            creationTarget,
            cancellationToken,
            null
        );

    private async ValueTask<bool> ReleaseEndedReservationAsync(
        ZLinkAuthorityKey key,
        string expectedStoreVersion,
        ZLinkCreationAuthorityRecovery? creationTarget,
        CancellationToken cancellationToken,
        ZLinkLocationOwnerToken? requiredOwner
    )
    {
        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            var current = await ReadAuthorityRecordAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (current is null || current.Snapshot.StoreVersion != expectedStoreVersion)
                return false;
            var result = await TryReclaimStaleAuthorityAsync(
                    current,
                    cancellationToken,
                    creationTarget,
                    requiredOwner: requiredOwner
                )
                .ConfigureAwait(false);
            if (result != StaleAuthorityReclaimResult.Conflict)
                return result == StaleAuthorityReclaimResult.Reclaimed;
        }
    }

    public ValueTask<ZLinkObjectCommitResult> CommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        CancellationToken cancellationToken = default
    ) => CompleteCommitAsync(reservation, readyPayload, null, cancellationToken);

    public ValueTask<ZLinkObjectCommitResult> CommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        DateTimeOffset operationDeadline,
        CancellationToken cancellationToken = default
    ) => CompleteCommitAsync(reservation, readyPayload, operationDeadline, cancellationToken);

    public ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
        ZLinkObjectReservation reservation,
        ZLinkObjectCreationCompletion completion,
        CancellationToken cancellationToken = default
    ) => CompleteCreationCoreAsync(reservation, completion, cancellationToken);

    private async ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationCoreAsync(
        ZLinkObjectReservation reservation,
        ZLinkObjectCreationCompletion completion,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        ArgumentNullException.ThrowIfNull(reservation);
        ArgumentNullException.ThrowIfNull(completion);
        var publication = completion switch
        {
            ZLinkObjectCreationCompletion.Created value => value.Terminal,
            ZLinkObjectCreationCompletion.Rejected value => value.Terminal,
            ZLinkObjectCreationCompletion.Failed value => value.Terminal,
            _ => throw new ArgumentOutOfRangeException(nameof(completion)),
        };
        ValidateTerminal(publication);
        var terminalKey = TerminalKey(publication.Operation);
        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            var existingTerminal = await ReadTerminalAsync(publication.Operation, cancellationToken)
                .ConfigureAwait(false);
            if (existingTerminal is not null)
                return new ZLinkObjectCreationCompleteResult.AlreadyCompleted(
                    existingTerminal.Record
                );

            var current = await ReadAuthorityRecordAsync(reservation.Key, cancellationToken)
                .ConfigureAwait(false);
            if (!MatchesReservation(current, reservation))
                return new ZLinkObjectCreationCompleteResult.Stale();
            if (publication.ExpiresAt <= current!.Snapshot.StoreNow)
                throw new ArgumentOutOfRangeException(
                    nameof(completion),
                    "The creation terminal must expire after StoreNow."
                );
            var target = await ReadEligibleTargetAsync(
                    reservation.TargetDescriptor,
                    reservation.TargetNodeLifecycleGeneration,
                    reservation.TargetOwner,
                    current!.Snapshot.Allocation.ObjectKind,
                    current.Snapshot.Allocation.StableType,
                    cancellationToken,
                    requireNewPlacementEligibility: false
                )
                .ConfigureAwait(false);
            if (target is null)
                return new ZLinkObjectCreationCompleteResult.Stale();
            await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
            var terminal = new ZLinkCreationTerminalRecord(
                publication.Operation,
                publication.TerminalEnvelope.ToArray(),
                publication.ExpiresAt,
                current.Snapshot.StoreNow
            );
            var conditions = new List<ZLinkStoreCondition>
            {
                new ZLinkStoreCondition.Version(AuthorityMetaKey(reservation.Key), current.Version),
                new ZLinkStoreCondition.Missing(terminalKey),
                target.DescriptorCondition,
                target.OwnerCondition,
            };
            var mutations = new List<ZLinkStoreMutation>
            {
                new ZLinkStoreMutation.Put(
                    terminalKey,
                    publication.TerminalEnvelope.ToArray(),
                    publication.ExpiresAt - current.Snapshot.StoreNow
                ),
            };
            AddCapacityMutations(
                mutations,
                current.Snapshot.Allocation,
                pending: -1,
                active: completion is ZLinkObjectCreationCompletion.Created ? 1 : 0
            );
            AuthorityMeta? readyMeta = null;
            if (completion is ZLinkObjectCreationCompletion.Created created)
            {
                ValidateAuthorityPayload(created.ReadyPayload);
                if (
                    (
                        await AddMembershipCommitConditionsAsync(
                                created.ReadyPayload,
                                current.Snapshot,
                                conditions,
                                cancellationToken
                            )
                            .ConfigureAwait(false)
                    )
                    is not null
                )
                    return new ZLinkObjectCreationCompleteResult.Stale();
                readyMeta = current.Meta with
                {
                    Payload = created.ReadyPayload.ToArray(),
                    Allocation = current.Snapshot.Allocation with
                    {
                        State = ZLinkPlacementAllocationState.Active,
                    },
                    ReservedCreation = null,
                };
                mutations.Add(
                    new ZLinkStoreMutation.Put(
                        AuthorityMetaKey(reservation.Key),
                        Encode(readyMeta),
                        null
                    )
                );
            }
            else
            {
                mutations.Add(new ZLinkStoreMutation.Delete(AuthorityMetaKey(reservation.Key)));
            }
            var result = await _provider
                .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Conflict)
                continue;
            var applied = (ZLinkStoreWriteResult.Applied)result;
            terminal = terminal with { StoreNow = applied.StoreNow };
            return completion switch
            {
                ZLinkObjectCreationCompletion.Created =>
                    new ZLinkObjectCreationCompleteResult.Created(
                        Snapshot(
                            readyMeta!,
                            applied.PutVersions[AuthorityMetaKey(reservation.Key)],
                            applied.StoreNow,
                            ((ZLinkObjectCreationCompletion.Created)completion).ReadyPayload
                        ),
                        terminal
                    ),
                ZLinkObjectCreationCompletion.Rejected =>
                    new ZLinkObjectCreationCompleteResult.Rejected(terminal),
                _ => new ZLinkObjectCreationCompleteResult.Failed(terminal),
            };
        }
    }

    public async ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
        ZLinkCreationOperationId operation,
        CancellationToken cancellationToken = default
    )
    {
        ValidateCreationOperation(operation);
        var terminal = await ReadTerminalAsync(operation, cancellationToken).ConfigureAwait(false);
        return terminal is null
            ? new ZLinkCreationTerminalReadResult.Missing(
                await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false)
            )
            : new ZLinkCreationTerminalReadResult.Found(terminal.Record);
    }

    public async ValueTask<ZLinkObjectAbortResult> AbortAsync(
        ZLinkObjectReservation reservation,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentNullException.ThrowIfNull(reservation);
        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            var current = await ReadAuthorityRecordAsync(reservation.Key, cancellationToken)
                .ConfigureAwait(false);
            if (!MatchesReservation(current, reservation))
                return new ZLinkObjectAbortResult.Stale();
            var target = await ReadEligibleTargetAsync(
                    reservation.TargetDescriptor,
                    reservation.TargetNodeLifecycleGeneration,
                    reservation.TargetOwner,
                    current!.Snapshot.Allocation.ObjectKind,
                    current.Snapshot.Allocation.StableType,
                    cancellationToken,
                    requireNewPlacementEligibility: false
                )
                .ConfigureAwait(false);
            if (target is null)
                return new ZLinkObjectAbortResult.Stale();
            await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
            var mutations = new List<ZLinkStoreMutation>
            {
                new ZLinkStoreMutation.Delete(AuthorityMetaKey(reservation.Key)),
            };
            AddCapacityMutations(mutations, current!.Snapshot.Allocation, pending: -1);
            var result = await _provider
                .WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [
                            new ZLinkStoreCondition.Version(
                                AuthorityMetaKey(reservation.Key),
                                current.Version
                            ),
                            target.DescriptorCondition,
                            target.OwnerCondition,
                        ],
                        mutations
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Conflict)
                continue;
            return new ZLinkObjectAbortResult.Aborted();
        }
    }

    public async ValueTask RemoveSealedInventoryAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    )
    {
        var key = AggregateKey(fence);
        var aggregate = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
            .ConfigureAwait(false);
        if (aggregate is null)
        {
            await ZLinkAggregateInventory
                .RemoveInventoryAsync(_provider, Prefix, fence, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await CleanupAggregateParticipantsAsync(fence, cancellationToken).ConfigureAwait(false);
        aggregate = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
            .ConfigureAwait(false);
        if (aggregate?.Record.ParticipantsCleaned != true)
            return;

        await _provider
            .WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(key, aggregate.Version)],
                    [
                        new ZLinkStoreMutation.Delete(AggregateHousekeepingKey(key)),
                        new ZLinkStoreMutation.Delete(key),
                    ]
                ),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public ValueTask<string> SealAggregateInventoryAsync(
        ZLinkAggregateFence fence,
        IReadOnlyList<ZLinkAggregateParticipant> participants,
        CancellationToken cancellationToken
    ) =>
        ZLinkAggregateInventory.SealAsync(
            _provider,
            Prefix,
            fence,
            participants,
            cancellationToken
        );

    public ValueTask<
        IReadOnlyList<ZLinkAggregateInventory.Participant>
    > ReadAggregateInventoryAsync(
        ZLinkAggregateFence fence,
        string digest,
        CancellationToken cancellationToken
    ) => ZLinkAggregateInventory.ReadAsync(_provider, Prefix, fence, digest, cancellationToken);

    public async ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
        ZLinkAggregatePrepareRequest request,
        CancellationToken cancellationToken = default
    )
    {
        ValidateAggregate(request);
        request = CanonicalizeAggregateRequest(request);
        await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
        await EnsureAggregateStagingRecoveryAsync(cancellationToken).ConfigureAwait(false);
        var retryDeadline = Stopwatch.GetElapsedTime(0) + WriteConflictRetryWindow;
        var fence = new ZLinkAggregateFence(request.AggregateId, request.AggregateGeneration);
        var key = AggregateKey(fence);
        var inventoryParticipants = ZLinkAggregateInventory.Prepare(request.Participants);
        var inventory = ZLinkAggregateInventory.Build(
            inventoryParticipants.Select(static participant => participant.Entry).ToArray()
        );
        request = request with
        {
            InventoryDigest = Convert.FromHexString(inventory.Root.DigestSha256),
        };
        var header = CreateAggregateHeader(request);
        var requestFingerprint = AggregateRequestFingerprint(header, request.Participants.Count);
        var existing = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
            .ConfigureAwait(false);
        if (existing is not null && existing.Record.Status != AggregateStatus.Staging)
            return await ReconcileExistingAggregateAsync(
                    fence,
                    existing.Record,
                    request,
                    cancellationToken
                )
                .ConfigureAwait(false);
        if (
            existing is not null
            && (
                !AggregateHeaderEquals(existing.Record.Header, header)
                || !CryptographicOperations.FixedTimeEquals(
                    existing.Record.RequestFingerprint,
                    requestFingerprint
                )
            )
        )
            return new ZLinkAggregatePrepareResult.Conflict();
        var staging = existing;

        var requiresPlacement = request.Participants.Any(static participant =>
            participant.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner
        );
        ValueTask<StoredTarget?> ReadTargetAsync() =>
            ReadEligibleTargetAsync(
                request.TargetDescriptor,
                request.TargetDescriptorLifecycleGeneration,
                request.TargetOwner,
                null,
                null,
                cancellationToken,
                requireNewPlacementEligibility: requiresPlacement,
                allowPreparingTarget: request.AllowPreparingTarget
            );

        var target = await ReadTargetAsync().ConfigureAwait(false);
        if (target is null)
            return new ZLinkAggregatePrepareResult.Conflict();
        var authorities = new StoredAuthority?[request.Participants.Count];
        var ownerCounters = new GenerationCounter?[request.Participants.Count];
        await Parallel
            .ForEachAsync(
                Enumerable.Range(0, request.Participants.Count),
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (index, token) =>
                {
                    var participant = request.Participants[index];
                    var authority = await ReadAuthorityRecordAsync(participant.Key, token)
                        .ConfigureAwait(false);
                    authorities[index] = authority;
                    if (
                        authority is not null
                        && authority.Meta.AggregateFence is null
                        && participant.OwnerTransition
                            == ZLinkAuthorityGenerationTransition.NewOwner
                    )
                        ownerCounters[index] = await ReadGenerationCounterAsync(
                                AuthorityOwnerGenerationCounterKey(participant.Key),
                                token
                            )
                            .ConfigureAwait(false);
                }
            )
            .ConfigureAwait(false);
        for (var index = 0; index < request.Participants.Count; index++)
        {
            var participant = request.Participants[index];
            var authority = authorities[index];
            var ownsExistingFence =
                authority is not null
                && authority.Meta.AggregateFence == fence
                && authority.Meta.AggregateParticipantIndex == index
                && string.Equals(
                    authority.Meta.AggregateExpectedStoreVersion,
                    participant.ExpectedStoreVersion,
                    StringComparison.Ordinal
                );
            if (
                authority is null
                || !ownsExistingFence
                    && (
                        authority.Version.Value != participant.CompareExchangeStoreVersion
                        || authority.Meta.AggregateFence is not null
                    )
                || participant.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner
                    && !IsEligible(target.Descriptor, authority.Snapshot.Allocation)
            )
                return new ZLinkAggregatePrepareResult.Conflict();
        }
        if (ownerCounters.Any(static counter => counter?.Value == MaximumGeneration))
            return new ZLinkAggregatePrepareResult.GenerationExhausted();
        var capacityConditions = CapacityReservationConditions(target.Descriptor, request.Capacity);
        if (capacityConditions is null)
            return new ZLinkAggregatePrepareResult.Conflict();
        try
        {
            staging ??= await ClaimAggregateStagingAsync(
                    key,
                    header,
                    request.Participants.Count,
                    requestFingerprint,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (staging is null)
            {
                var raced = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                    .ConfigureAwait(false);
                if (raced is null)
                    return new ZLinkAggregatePrepareResult.Conflict();
                if (raced.Record.Status != AggregateStatus.Staging)
                    return await ReconcileExistingAggregateAsync(
                            fence,
                            raced.Record,
                            request,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                if (
                    !AggregateHeaderEquals(raced.Record.Header, header)
                    || !CryptographicOperations.FixedTimeEquals(
                        raced.Record.RequestFingerprint,
                        requestFingerprint
                    )
                )
                    return new ZLinkAggregatePrepareResult.Conflict();
                staging = raced;
            }
            await ZLinkAggregateInventory
                .WriteAsync(
                    _provider,
                    Prefix,
                    fence,
                    inventory,
                    inventoryParticipants,
                    cancellationToken
                )
                .ConfigureAwait(false);
            var stagedParticipants = request
                .Participants.Select(
                    (participant, index) =>
                        new AggregateParticipantRecord(
                            index,
                            participant.Key.Value,
                            participant.ExpectedStoreVersion,
                            participant.OwnerTransition,
                            0,
                            requestFingerprint
                        )
                )
                .ToArray();
            if (
                !await StageAggregateParticipantsAsync(fence, stagedParticipants, cancellationToken)
                    .ConfigureAwait(false)
            )
            {
                await AbortAggregateStagingAsync(fence, staging, cancellationToken)
                    .ConfigureAwait(false);
                return new ZLinkAggregatePrepareResult.Conflict();
            }
            var generations = await InstallAggregateParticipantFencesAsync(
                    fence,
                    request,
                    authorities,
                    ownerCounters,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (generations.Contains(0UL))
            {
                await AbortAggregateStagingAsync(fence, staging, cancellationToken)
                    .ConfigureAwait(false);
                return new ZLinkAggregatePrepareResult.Conflict();
            }
            var mutations = new List<ZLinkStoreMutation>
            {
                new ZLinkStoreMutation.Put(
                    key,
                    Encode(staging.Record with { Status = AggregateStatus.Prepared }),
                    null
                ),
            };
            ZLinkStoreCondition.Version? representativeVersion = null;
            if (request.RepresentativeKey is { } representative)
            {
                var primary = await ReadAuthorityRecordAsync(
                        representative,
                        knownMeta: null,
                        projectCommittedAggregate: false,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                var progress = primary?.Snapshot.Relocation;
                if (
                    primary is null
                    || primary.Meta.AggregateFence != fence
                    || progress is not { Phase: ServiceWireCodec.RelocationPhase.Captured }
                    || request.Relocation?.Target is not { } attempt
                    || !progress.HoldsRestoreSource(
                        request.AggregateId,
                        primary.Snapshot,
                        request.Relocation.Source,
                        request.Relocation.Coordinator.ExpectedStoreVersion
                    )
                    || !progress.AcceptsAttempt(attempt)
                )
                {
                    var observed = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                        .ConfigureAwait(false);
                    if (
                        observed?.Record.Status
                        is AggregateStatus.Prepared
                            or AggregateStatus.Committed
                    )
                        return await ReconcileExistingAggregateAsync(
                                fence,
                                observed.Record,
                                request,
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                    await AbortAggregateStagingAsync(fence, staging, cancellationToken)
                        .ConfigureAwait(false);
                    return new ZLinkAggregatePrepareResult.Conflict();
                }

                var authorityKey = AuthorityMetaKey(representative);
                representativeVersion = new(authorityKey, primary.Version);
                mutations.Add(
                    new ZLinkStoreMutation.Put(
                        authorityKey,
                        Encode(
                            primary.Meta with
                            {
                                Aggregate = ZLinkAuthorityAggregateCodec.Replace(
                                    primary.Meta.Aggregate,
                                    progress with
                                    {
                                        Phase = ServiceWireCodec.RelocationPhase.Prepared,
                                        Target = attempt,
                                    }
                                ),
                            }
                        ),
                        null
                    )
                );
            }
            AddCapacityMutations(mutations, request.TargetDescriptor, request.Capacity, pending: 1);
            ZLinkStoreWriteResult result;
            try
            {
                for (var retry = 0; ; retry++)
                {
                    var conditions = new List<ZLinkStoreCondition>
                    {
                        new ZLinkStoreCondition.Version(key, staging.Version),
                        target.DescriptorCondition,
                        target.OwnerCondition,
                    };
                    if (representativeVersion is not null)
                        conditions.Add(representativeVersion);
                    conditions.AddRange(capacityConditions);
                    result = await _provider
                        .WriteAsync(
                            new ZLinkStoreWriteRequest(conditions, mutations),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    if (
                        result is not ZLinkStoreWriteResult.Conflict
                        || retry >= WriteConflictRetryLimit
                        || Stopwatch.GetElapsedTime(0) >= retryDeadline
                    )
                        break;

                    var sameAuthority = await ReservationAuthorityMatchesAsync(
                            new ZLinkStoreCondition.Version(key, staging.Version),
                            target,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    if (sameAuthority)
                    {
                        if (
                            capacityConditions.Count == 0
                            || await IsReservationCapacityExceededAsync(
                                    target.Descriptor,
                                    capacityConditions,
                                    cancellationToken
                                )
                                .ConfigureAwait(false)
                        )
                            break;
                    }
                    else
                    {
                        // A renewed descriptor can still admit the staged aggregate.
                        var refreshed = await ReadTargetAsync().ConfigureAwait(false);
                        if (
                            refreshed is null
                            || refreshed.DescriptorCondition == target.DescriptorCondition
                        )
                            break;
                        var refreshedCapacity = CapacityReservationConditions(
                            refreshed.Descriptor,
                            request.Capacity
                        );
                        if (refreshedCapacity is null)
                            break;
                        target = refreshed;
                        capacityConditions = refreshedCapacity;
                    }
                    await DelayWriteConflictRetryAsync(retry, retryDeadline, cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            catch
            {
                var reconciled = await ReadRecordForReconciliationAsync<AggregateRecord>(key)
                    .ConfigureAwait(false);
                if (
                    reconciled is not null
                    && reconciled.Record.Status
                        is AggregateStatus.Prepared
                            or AggregateStatus.Committed
                )
                    return await ReconcileExistingAggregateForAmbiguousAsync(
                            fence,
                            reconciled.Record,
                            request
                        )
                        .ConfigureAwait(false);
                throw;
            }
            if (result is ZLinkStoreWriteResult.Applied)
                return new ZLinkAggregatePrepareResult.Prepared(fence)
                {
                    TargetAuthorityOwnerGenerations = request
                        .Participants.Select(
                            (participant, index) =>
                                (participant.Key, Generation: generations[index])
                        )
                        .ToDictionary(static entry => entry.Key, static entry => entry.Generation),
                };
            var reconciliation = await ReconcileAggregatePrepareAsync(
                    key,
                    fence,
                    request,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (reconciliation is ZLinkAggregatePrepareResult.Conflict)
            {
                var stillStaging = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                    .ConfigureAwait(false);
                if (stillStaging?.Record.Status == AggregateStatus.Staging)
                    await AbortAggregateStagingAsync(fence, stillStaging, cancellationToken)
                        .ConfigureAwait(false);
            }
            return reconciliation;
        }
        catch
        {
            var recovered = await ReconcileOrAbortClaimedAggregatePrepareAsync(
                    fence,
                    header,
                    requestFingerprint,
                    request
                )
                .ConfigureAwait(false);
            if (recovered is not null)
                return recovered;
            throw;
        }
    }

    public ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default
    ) => CommitAggregateCoreAsync(fence, cancellationToken);

    public ValueTask<ZLinkAggregateCommitResult> CommitAggregateForRecoveryAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default
    ) => CommitAggregateAsync(fence, cancellationToken);

    public ValueTask<ZLinkAggregateCommitResult> CommitAggregateAttemptAsync(
        ZLinkAggregateFence fence,
        ulong attempt,
        CancellationToken cancellationToken
    ) => CommitAggregateCoreAsync(fence, cancellationToken, attempt);

    private async ValueTask<ZLinkAggregateCommitResult> CommitAggregateCoreAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken,
        ulong? attempt = null
    )
    {
        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            var key = AggregateKey(fence);
            var aggregate = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                .ConfigureAwait(false);
            if (aggregate is null || aggregate.Record.Status == AggregateStatus.Aborted)
                return ZLinkAggregateCommitResult.Stale;
            if (aggregate.Record.Status == AggregateStatus.Staging)
                return ZLinkAggregateCommitResult.Stale;
            if (aggregate.Record.Status == AggregateStatus.Committed)
            {
                if (!MatchesCommittedAttempt(aggregate.Record, attempt))
                    return ZLinkAggregateCommitResult.Stale;
                await NormalizeCommittedAggregateAsync(fence, aggregate.Record, cancellationToken)
                    .ConfigureAwait(false);
                return ZLinkAggregateCommitResult.AlreadyCommitted;
            }
            var participants = await ReadAggregateParticipantsAsync(
                    fence,
                    aggregate.Record,
                    cancellationToken
                )
                .ConfigureAwait(false);
            var request = RehydrateAggregateRequest(aggregate.Record, participants);
            var authorities = new StoredAuthority?[request.Participants.Count];
            await Parallel
                .ForEachAsync(
                    Enumerable.Range(0, request.Participants.Count),
                    new ParallelOptions
                    {
                        MaxDegreeOfParallelism = MaximumStoreConcurrency,
                        CancellationToken = cancellationToken,
                    },
                    async (index, token) =>
                    {
                        authorities[index] = await ReadAuthorityRecordAsync(
                                request.Participants[index].Key,
                                knownMeta: null,
                                projectCommittedAggregate: false,
                                token
                            )
                            .ConfigureAwait(false);
                    }
                )
                .ConfigureAwait(false);
            if (
                authorities.Any(authority =>
                    authority is null || authority.Meta.AggregateFence != fence
                )
            )
                return ZLinkAggregateCommitResult.Stale;
            await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
            var deltas = new Dictionary<ZLinkStoreKey, long>();
            AccumulateCapacity(deltas, request.TargetDescriptor, request.Capacity, pending: -1);

            var conditions = new List<ZLinkStoreCondition>
            {
                new ZLinkStoreCondition.Version(key, aggregate.Version),
            };
            var committed = aggregate.Record with { Status = AggregateStatus.Committed };
            if (request.RepresentativeKey is { } representative)
            {
                var authority = authorities.Single(item => item!.Key == representative)!;
                if (
                    authority.Snapshot.Relocation
                        is not { Phase: ServiceWireCodec.RelocationPhase.Prepared } relocation
                    || relocation.RelocationId != fence.AggregateId
                    || relocation.Target?.AttemptGeneration != attempt
                )
                    return ZLinkAggregateCommitResult.Stale;
                conditions.Add(
                    new ZLinkStoreCondition.Version(
                        AuthorityMetaKey(representative),
                        authority.Version
                    )
                );
                committed = committed with
                {
                    Header = committed.Header with
                    {
                        Relocation = relocation with
                        {
                            Phase = ServiceWireCodec.RelocationPhase.Committed,
                        },
                    },
                };
            }
            for (var index = 0; index < request.Participants.Count; index++)
                if (
                    (
                        await AddMembershipCommitConditionsAsync(
                                aggregate.Record.Header.ProjectPayload(
                                    authorities[index]!.Meta.Allocation.ObjectKind,
                                    request.Participants[index].AuthorityPayload
                                ),
                                authorities[index]!.Snapshot,
                                conditions,
                                cancellationToken,
                                request
                            )
                            .ConfigureAwait(false)
                    )
                    is not null
                )
                    return ZLinkAggregateCommitResult.Stale;
            var mutations = new List<ZLinkStoreMutation>();
            // authorities[index].Meta already carries both generation values
            // for this exact read (checklist C-2b: no separate GenerationKey
            // to reconcile), so the per-participant next-generation check below
            // compares directly against it.
            for (var index = 0; index < request.Participants.Count; index++)
            {
                var participant = request.Participants[index];
                var authority = authorities[index]!;
                var changesOwner =
                    participant.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner;
                if (changesOwner)
                {
                    ulong nextAuthorityOwnerGeneration = participants[index]
                        .Metadata
                        .TargetAuthorityOwnerGeneration;
                    if (
                        nextAuthorityOwnerGeneration == 0
                        || nextAuthorityOwnerGeneration <= authority.Meta.AuthorityOwnerGeneration
                        || nextAuthorityOwnerGeneration > long.MaxValue
                    )
                        return ZLinkAggregateCommitResult.Stale;
                    var source = authority.Snapshot.Allocation;
                    AccumulateCapacity(deltas, source.Descriptor, source.Capacity, active: -1);
                    AccumulateCapacity(
                        deltas,
                        request.TargetDescriptor,
                        source.Capacity,
                        active: 1
                    );
                }
            }
            AppendCapacityMutations(mutations, deltas);
            mutations.Add(new ZLinkStoreMutation.Put(key, Encode(committed), null));
            if (request.RepresentativeKey is not null)
                mutations.Add(
                    new ZLinkStoreMutation.Put(
                        AggregateHousekeepingKey(key),
                        EncodeAggregateHousekeeping(committed),
                        null
                    )
                );
            ZLinkStoreWriteResult result;
            try
            {
                result = await _provider
                    .WriteAsync(
                        new ZLinkStoreWriteRequest(conditions, mutations),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            catch
            {
                var reconciled = await ReadRecordForReconciliationAsync<AggregateRecord>(key)
                    .ConfigureAwait(false);
                if (reconciled?.Record.Status != AggregateStatus.Committed)
                    throw;
                if (!MatchesCommittedAttempt(reconciled.Record, attempt))
                    return ZLinkAggregateCommitResult.Stale;
                await NormalizeCommittedAggregateForAmbiguousAsync(fence, reconciled.Record)
                    .ConfigureAwait(false);
                return ZLinkAggregateCommitResult.AlreadyCommitted;
            }
            if (result is not ZLinkStoreWriteResult.Applied)
            {
                var reconciled = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                    .ConfigureAwait(false);
                if (reconciled?.Record.Status == AggregateStatus.Committed)
                {
                    if (!MatchesCommittedAttempt(reconciled.Record, attempt))
                        return ZLinkAggregateCommitResult.Stale;
                    await NormalizeCommittedAggregateAsync(
                            fence,
                            reconciled.Record,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    return ZLinkAggregateCommitResult.AlreadyCommitted;
                }
                if (reconciled?.Record.Status == AggregateStatus.Prepared)
                {
                    continue;
                }
                return ZLinkAggregateCommitResult.Stale;
            }
            await NormalizeCommittedAggregateAsync(fence, committed, cancellationToken)
                .ConfigureAwait(false);
            return ZLinkAggregateCommitResult.Committed;
        }
    }

    private static bool MatchesCommittedAttempt(AggregateRecord aggregate, ulong? attempt) =>
        aggregate.Header.RepresentativeKey is null
        || aggregate.Header.Relocation?.Target?.AttemptGeneration == attempt;

    private async ValueTask NormalizeCommittedAggregateAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken
    )
    {
        if (aggregate.Status != AggregateStatus.Committed)
            throw new ZLinkRelocationDataLostException(
                $"Aggregate '{fence.AggregateId:N}' is not a committed authority publication."
            );
        if (await IsAggregateRootMissingAsync(fence, cancellationToken).ConfigureAwait(false))
            return;
        if (aggregate.ParticipantsNormalized)
        {
            if (aggregate.Header.RepresentativeKey is null)
                await CleanupAggregateParticipantsAsync(fence, cancellationToken)
                    .ConfigureAwait(false);
            return;
        }

        var storedParticipants = await ReadAggregateParticipantsAsync(
                fence,
                aggregate,
                cancellationToken
            )
            .ConfigureAwait(false);
        var request = RehydrateAggregateRequest(aggregate, storedParticipants);
        await Parallel
            .ForEachAsync(
                storedParticipants,
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (storedParticipant, token) =>
                {
                    var participant = request.Participants[storedParticipant.Metadata.Index];
                    var normalized = false;
                    for (var attempt = 0; attempt < 8; attempt++)
                    {
                        var authority =
                            await ReadAuthorityRecordAsync(
                                    participant.Key,
                                    knownMeta: null,
                                    projectCommittedAggregate: false,
                                    token
                                )
                                .ConfigureAwait(false)
                            ?? throw new ZLinkRelocationDataLostException(
                                $"Committed aggregate authority '{participant.Key.Value}' is missing."
                            );
                        if (authority.Meta.AggregateFence is null)
                        {
                            if (
                                !IsNormalizedAggregateParticipant(
                                    authority,
                                    participant with
                                    {
                                        AuthorityPayload = aggregate.Header.ProjectPayload(
                                            authority.Meta.Allocation.ObjectKind,
                                            participant.AuthorityPayload
                                        ),
                                    },
                                    aggregate,
                                    storedParticipant.Metadata
                                )
                            )
                                throw new ZLinkRelocationDataLostException(
                                    $"Committed aggregate authority '{participant.Key.Value}' changed during normalization."
                                );
                            normalized = true;
                            break;
                        }
                        if (authority.Meta.AggregateFence != fence)
                            throw new ZLinkRelocationDataLostException(
                                $"Committed aggregate authority '{participant.Key.Value}' has a different fence."
                            );

                        var changesOwner =
                            participant.OwnerTransition
                            == ZLinkAuthorityGenerationTransition.NewOwner;
                        var nextGeneration = authority.Meta.AuthorityOwnerGeneration;
                        var conditions = new List<ZLinkStoreCondition>
                        {
                            new ZLinkStoreCondition.Version(
                                AuthorityMetaKey(participant.Key),
                                authority.Version
                            ),
                        };
                        var mutations = new List<ZLinkStoreMutation>();
                        if (changesOwner)
                        {
                            // authority.Meta already carries both generation values
                            // for this exact read (checklist C-2b: no separate
                            // GenerationKey to reconcile).
                            if (
                                (
                                    nextGeneration = storedParticipant
                                        .Metadata
                                        .TargetAuthorityOwnerGeneration
                                ) == 0
                                || nextGeneration <= authority.Meta.AuthorityOwnerGeneration
                                || nextGeneration > long.MaxValue
                            )
                                throw new ZLinkRelocationDataLostException(
                                    $"Committed aggregate authority '{participant.Key.Value}' lost its generation fence."
                                );
                        }

                        var allocation = changesOwner
                            ? authority.Meta.Allocation with
                            {
                                Descriptor = request.TargetDescriptor,
                                DescriptorLifecycleGeneration =
                                    request.TargetDescriptorLifecycleGeneration,
                            }
                            : authority.Meta.Allocation;
                        mutations.Add(
                            new ZLinkStoreMutation.Put(
                                AuthorityMetaKey(participant.Key),
                                Encode(
                                    authority.Meta with
                                    {
                                        Payload = aggregate
                                            .Header.ProjectPayload(
                                                authority.Meta.Allocation.ObjectKind,
                                                participant.AuthorityPayload
                                            )
                                            .ToArray(),
                                        Aggregate = aggregate.Header.ApplyRelocation(
                                            authority.Meta.Aggregate
                                        ),
                                        AuthorityOwnerGeneration = nextGeneration,
                                        OwnerId = changesOwner
                                            ? request.TargetOwner.OwnerId
                                            : authority.Meta.OwnerId,
                                        OwnerLeaseGeneration = changesOwner
                                            ? request.TargetOwner.LeaseGeneration
                                            : authority.Meta.OwnerLeaseGeneration,
                                        Allocation = allocation,
                                        AggregateFence = null,
                                        AggregateParticipantIndex = null,
                                        AggregateExpectedStoreVersion = null,
                                    }
                                ),
                                null
                            )
                        );
                        var result = await _provider
                            .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), token)
                            .ConfigureAwait(false);
                        if (result is ZLinkStoreWriteResult.Applied)
                        {
                            normalized = true;
                            break;
                        }
                    }
                    if (!normalized)
                        throw new IOException(
                            $"Committed aggregate authority '{participant.Key.Value}' could not be normalized."
                        );
                }
            )
            .ConfigureAwait(false);
        await MarkAggregateParticipantsNormalizedAsync(fence, cancellationToken)
            .ConfigureAwait(false);
        if (aggregate.Header.RepresentativeKey is null)
            await CleanupAggregateParticipantsAsync(fence, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask MarkAggregateParticipantsNormalizedAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    )
    {
        var key = AggregateKey(fence);
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var aggregate =
                await ReadRecordAsync<AggregateRecord>(key, cancellationToken).ConfigureAwait(false)
                ?? throw new ZLinkRelocationDataLostException(
                    $"Committed aggregate '{fence.AggregateId:N}' is missing."
                );
            if (aggregate.Record.ParticipantsNormalized)
                return;
            if (aggregate.Record.Status != AggregateStatus.Committed)
                throw new ZLinkRelocationDataLostException(
                    $"Aggregate '{fence.AggregateId:N}' is not committed."
                );
            var result = await _provider
                .WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Version(key, aggregate.Version)],
                        [
                            new ZLinkStoreMutation.Put(key, Encode(aggregate.Record), null),
                            new ZLinkStoreMutation.Put(
                                AggregateHousekeepingKey(key),
                                EncodeAggregateHousekeeping(
                                    aggregate.Record with
                                    {
                                        ParticipantsNormalized = true,
                                    }
                                ),
                                null
                            ),
                        ]
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Applied)
                return;
        }
        throw new IOException(
            $"Aggregate '{fence.AggregateId:N}' normalization marker could not be stored."
        );
    }

    private async ValueTask CleanupAggregateParticipantsAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    )
    {
        var key = AggregateKey(fence);
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var aggregate = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                .ConfigureAwait(false);
            if (aggregate is null)
                return;
            var record = aggregate.Record;
            if (record.Status is not (AggregateStatus.Committed or AggregateStatus.Aborted))
                return;
            var rootMissing = await IsAggregateRootMissingAsync(fence, cancellationToken)
                .ConfigureAwait(false);
            if (
                record.Status == AggregateStatus.Committed
                && !record.ParticipantsNormalized
                && !rootMissing
            )
                return;

            if (!record.ParticipantsCleaned)
            {
                var result = await _provider
                    .WriteAsync(
                        new ZLinkStoreWriteRequest(
                            [new ZLinkStoreCondition.Version(key, aggregate.Version)],
                            [
                                new ZLinkStoreMutation.Put(key, Encode(record), null),
                                new ZLinkStoreMutation.Put(
                                    AggregateHousekeepingKey(key),
                                    EncodeAggregateHousekeeping(
                                        record with
                                        {
                                            ParticipantsCleaned = true,
                                        }
                                    ),
                                    null
                                ),
                            ]
                        ),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (result is not ZLinkStoreWriteResult.Applied)
                    continue;
            }

            await CleanupUnfencedAggregateChildrenAsync(fence, cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        throw new IOException(
            $"Aggregate '{fence.AggregateId:N}' cleanup marker could not be stored."
        );
    }

    private async ValueTask<bool> IsAggregateRootMissingAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    ) =>
        await _provider
            .ReadAsync(ZLinkAggregateInventory.RootKey(Prefix, fence), cancellationToken)
            .ConfigureAwait(false) is ZLinkStoreReadResult.Missing;

    private static bool IsNormalizedAggregateParticipant(
        StoredAuthority authority,
        ZLinkAggregateParticipant participant,
        AggregateRecord aggregate,
        AggregateParticipantRecord storedParticipant
    )
    {
        var changesOwner =
            participant.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner;
        if (
            !CryptographicOperations.FixedTimeEquals(
                authority.Snapshot.Payload.Span,
                participant.AuthorityPayload.Span
            )
        )
            return false;
        if (
            aggregate.Header.ClearRelocation && authority.Snapshot.Relocation is not null
            || aggregate.Header.Relocation is { } relocation
                && authority.Snapshot.Relocation != relocation
        )
            return false;
        if (!changesOwner)
            return true;
        var generation = storedParticipant.TargetAuthorityOwnerGeneration;
        return generation > 0
            && authority.Meta.AuthorityOwnerGeneration == generation
            && authority.Meta.OwnerId == aggregate.Header.TargetOwner.OwnerId
            && authority.Meta.OwnerLeaseGeneration == aggregate.Header.TargetOwner.LeaseGeneration
            && authority.Meta.Allocation.Descriptor == aggregate.Header.TargetDescriptor
            && authority.Meta.Allocation.DescriptorLifecycleGeneration
                == aggregate.Header.TargetDescriptorLifecycleGeneration;
    }

    private async ValueTask<StoredRecord<AggregateRecord>?> ClaimAggregateStagingAsync(
        ZLinkStoreKey key,
        AggregateHeader header,
        int participantCount,
        byte[] requestFingerprint,
        CancellationToken cancellationToken
    )
    {
        var storeNow = await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false);
        var staged = new AggregateRecord(
            header,
            participantCount,
            requestFingerprint,
            AggregateStatus.Staging,
            storeNow,
            false,
            false
        );
        try
        {
            var claimed = await _provider
                .WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Missing(key)],
                        [
                            new ZLinkStoreMutation.Put(key, Encode(staged), null),
                            new ZLinkStoreMutation.Put(
                                AggregateHousekeepingKey(key),
                                EncodeAggregateHousekeeping(staged),
                                null
                            ),
                        ]
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            return claimed is ZLinkStoreWriteResult.Applied applied
                ? new StoredRecord<AggregateRecord>(
                    staged,
                    applied.PutVersions[key],
                    applied.StoreNow
                )
                : null;
        }
        catch
        {
            var reconciled = await ReadRecordForReconciliationAsync<AggregateRecord>(key)
                .ConfigureAwait(false);
            if (
                reconciled is not null
                && reconciled.Record.Status == AggregateStatus.Staging
                && AggregateHeaderEquals(reconciled.Record.Header, header)
                && CryptographicOperations.FixedTimeEquals(
                    reconciled.Record.RequestFingerprint,
                    requestFingerprint
                )
            )
                return reconciled;
            throw;
        }
    }

    private async ValueTask<bool> AbortAggregateStagingAsync(
        ZLinkAggregateFence fence,
        StoredRecord<AggregateRecord> staging,
        CancellationToken cancellationToken
    )
    {
        if (staging.Record.Status != AggregateStatus.Staging)
            return false;
        var key = AggregateKey(fence);
        var rootKey = ZLinkAggregateInventory.RootKey(Prefix, fence);
        if (
            await _provider.ReadAsync(rootKey, cancellationToken).ConfigureAwait(false)
            is ZLinkStoreReadResult.Missing
        )
        {
            if (staging.Record.Header.RepresentativeKey is not null)
                throw ZLinkAggregateInventory.DataLost(rootKey);
            var abandoned = await _provider
                .WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [
                            new ZLinkStoreCondition.Version(key, staging.Version),
                            new ZLinkStoreCondition.Missing(rootKey),
                        ],
                        [
                            new ZLinkStoreMutation.Put(
                                key,
                                Encode(staging.Record with { Status = AggregateStatus.Aborted }),
                                null
                            ),
                            new ZLinkStoreMutation.Put(
                                AggregateHousekeepingKey(key),
                                EncodeAggregateHousekeeping(
                                    staging.Record with
                                    {
                                        ParticipantsCleaned = true,
                                    }
                                ),
                                null
                            ),
                        ]
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (abandoned is ZLinkStoreWriteResult.Applied)
                await CleanupUnfencedAggregateChildrenAsync(fence, cancellationToken)
                    .ConfigureAwait(false);
            return abandoned is ZLinkStoreWriteResult.Applied;
        }
        var inventory = staging.Record.Header.RepresentativeKey is null
            ? null
            : await ReadAndValidateAggregateInventoryAsync(fence, staging.Record, cancellationToken)
                .ConfigureAwait(false);
        var result = await _provider
            .WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(key, staging.Version)],
                    [
                        new ZLinkStoreMutation.Put(
                            key,
                            Encode(staging.Record with { Status = AggregateStatus.Aborted }),
                            null
                        ),
                    ]
                ),
                cancellationToken
            )
            .ConfigureAwait(false);
        var aborted =
            result is ZLinkStoreWriteResult.Applied
                ? staging.Record with
                {
                    Status = AggregateStatus.Aborted,
                }
                : (
                    await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                        .ConfigureAwait(false)
                )?.Record;
        if (aborted?.Status != AggregateStatus.Aborted)
            return false;
        if (!aborted.ParticipantsCleaned)
        {
            await ReleaseAggregateParticipantFencesAsync(
                    fence,
                    aborted,
                    cancellationToken,
                    inventory
                )
                .ConfigureAwait(false);
            await CleanupAggregateParticipantsAsync(fence, cancellationToken).ConfigureAwait(false);
        }
        return result is ZLinkStoreWriteResult.Applied;
    }

    private async ValueTask<ZLinkAggregatePrepareResult?> ReconcileOrAbortClaimedAggregatePrepareAsync(
        ZLinkAggregateFence fence,
        AggregateHeader header,
        byte[] requestFingerprint,
        ZLinkAggregatePrepareRequest? request = null
    )
    {
        using var deadline = new CancellationTokenSource(AmbiguousReconciliationTimeout);
        try
        {
            for (var attempt = 0; attempt < 8; attempt++)
            {
                deadline.Token.ThrowIfCancellationRequested();
                var aggregate = await ReadRecordAsync<AggregateRecord>(
                        AggregateKey(fence),
                        deadline.Token
                    )
                    .ConfigureAwait(false);
                if (aggregate is null)
                    return null;
                if (
                    !AggregateHeaderEquals(aggregate.Record.Header, header)
                    || !CryptographicOperations.FixedTimeEquals(
                        aggregate.Record.RequestFingerprint,
                        requestFingerprint
                    )
                )
                    return new ZLinkAggregatePrepareResult.Conflict();

                if (
                    aggregate.Record.Status is AggregateStatus.Prepared or AggregateStatus.Committed
                )
                {
                    if (request is null)
                        return new ZLinkAggregatePrepareResult.Prepared(fence);
                    return await ReconcileExistingAggregateAsync(
                            fence,
                            aggregate.Record,
                            request,
                            deadline.Token
                        )
                        .ConfigureAwait(false);
                }

                if (aggregate.Record.Status == AggregateStatus.Aborted)
                {
                    try
                    {
                        await ReleaseAggregateParticipantFencesAsync(
                                fence,
                                aggregate.Record,
                                deadline.Token
                            )
                            .ConfigureAwait(false);
                        await CleanupAggregateParticipantsAsync(fence, deadline.Token)
                            .ConfigureAwait(false);
                    }
                    catch (ZLinkRelocationDataLostException)
                    {
                        if (
                            request is null
                            || await HasInstalledAggregateFenceAsync(
                                    fence,
                                    request.Participants,
                                    deadline.Token
                                )
                                .ConfigureAwait(false)
                        )
                            throw;

                        // The claim can be aborted before inventory staging.
                        // After every requested authority proves that this
                        // fence was never installed, remove any partially
                        // staged child rows without trusting the missing tree.
                        await CleanupUnfencedAggregateChildrenAsync(fence, deadline.Token)
                            .ConfigureAwait(false);
                    }
                    return new ZLinkAggregatePrepareResult.Conflict();
                }

                var abort = await _provider
                    .WriteAsync(
                        new ZLinkStoreWriteRequest(
                            [
                                new ZLinkStoreCondition.Version(
                                    AggregateKey(fence),
                                    aggregate.Version
                                ),
                            ],
                            [
                                new ZLinkStoreMutation.Put(
                                    AggregateKey(fence),
                                    Encode(
                                        aggregate.Record with
                                        {
                                            Status = AggregateStatus.Aborted,
                                        }
                                    ),
                                    null
                                ),
                            ]
                        ),
                        deadline.Token
                    )
                    .ConfigureAwait(false);
                if (abort is ZLinkStoreWriteResult.Applied)
                    continue;
            }
        }
        catch (ZLinkRelocationDataLostException)
        {
            throw;
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested)
        {
            return null;
        }
        catch
        {
            // A provider failure makes the durable result unknown. The
            // caller must preserve the immutable relocation root.
            return null;
        }
        return null;
    }

    private async ValueTask<bool> HasInstalledAggregateFenceAsync(
        ZLinkAggregateFence fence,
        IReadOnlyList<ZLinkAggregateParticipant> participants,
        CancellationToken cancellationToken
    )
    {
        var installed = new bool[participants.Count];
        await Parallel
            .ForEachAsync(
                Enumerable.Range(0, participants.Count),
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (index, token) =>
                {
                    var authority =
                        await ReadAuthorityRecordAsync(
                                participants[index].Key,
                                knownMeta: null,
                                projectCommittedAggregate: false,
                                token
                            )
                            .ConfigureAwait(false)
                        ?? throw new ZLinkRelocationDataLostException(
                            $"Aggregate '{fence.AggregateId:N}' participant authority '{participants[index].Key.Value}' is missing."
                        );
                    installed[index] = authority.Meta.AggregateFence == fence;
                }
            )
            .ConfigureAwait(false);
        return installed.Any(static value => value);
    }

    private async ValueTask CleanupUnfencedAggregateChildrenAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    )
    {
        await CleanupAggregateRowsAsync(
                AggregateKey(fence).Value + ":participant:",
                cancellationToken
            )
            .ConfigureAwait(false);
        await ZLinkAggregateInventory
            .RemoveInventoryAsync(_provider, Prefix, fence, cancellationToken)
            .ConfigureAwait(false);
    }

    private ValueTask CleanupAggregateRowsAsync(
        string childPrefix,
        CancellationToken cancellationToken
    ) => ZLinkAggregateInventory.RemoveAsync(_provider, childPrefix, cancellationToken);

    private async ValueTask ReleaseAggregateParticipantFencesAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken,
        IReadOnlyList<AggregateInventoryLeafEntry>? inventory = null
    )
    {
        inventory ??= await ReadAndValidateAggregateInventoryAsync(
                fence,
                aggregate,
                cancellationToken
            )
            .ConfigureAwait(false);
        await Parallel
            .ForEachAsync(
                inventory,
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (entry, token) =>
                {
                    var index = checked((int)entry.Index);
                    var key = new ZLinkAuthorityKey(entry.AuthorityKey);
                    for (var attempt = 0; attempt < 8; attempt++)
                    {
                        var authority = await ReadAuthorityRecordAsync(
                                key,
                                knownMeta: null,
                                projectCommittedAggregate: false,
                                token
                            )
                            .ConfigureAwait(false);
                        if (authority is null)
                            throw new ZLinkRelocationDataLostException(
                                $"Aggregate '{fence.AggregateId:N}' authority '{key.Value}' is missing while releasing its fence."
                            );
                        if (authority.Meta.AggregateFence is null)
                            return;
                        if (
                            authority.Meta.AggregateFence != fence
                            || authority.Meta.AggregateParticipantIndex != index
                            || !string.Equals(
                                authority.Meta.AggregateExpectedStoreVersion,
                                entry.ExpectedStoreVersion,
                                StringComparison.Ordinal
                            )
                        )
                            throw new ZLinkRelocationDataLostException(
                                $"Aggregate '{fence.AggregateId:N}' authority '{key.Value}' has a different fence."
                            );
                        var result = await _provider
                            .WriteAsync(
                                new ZLinkStoreWriteRequest(
                                    [
                                        new ZLinkStoreCondition.Version(
                                            AuthorityMetaKey(key),
                                            authority.Version
                                        ),
                                    ],
                                    [
                                        new ZLinkStoreMutation.Put(
                                            AuthorityMetaKey(key),
                                            Encode(
                                                authority.Meta with
                                                {
                                                    Aggregate =
                                                        key == aggregate.Header.RepresentativeKey
                                                            ? ZLinkAuthorityAggregateCodec.Replace(
                                                                authority.Meta.Aggregate,
                                                                null
                                                            )
                                                            : authority.Meta.Aggregate,
                                                    AggregateFence = null,
                                                    AggregateParticipantIndex = null,
                                                    AggregateExpectedStoreVersion = null,
                                                }
                                            ),
                                            null
                                        ),
                                    ]
                                ),
                                token
                            )
                            .ConfigureAwait(false);
                        if (result is ZLinkStoreWriteResult.Applied)
                            return;
                    }
                    throw new IOException(
                        $"Aggregate '{fence.AggregateId:N}' authority '{key.Value}' fence could not be released."
                    );
                }
            )
            .ConfigureAwait(false);
    }

    private async ValueTask EnsureAggregateStagingRecoveryAsync(CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref aggregateStagingRecoveryCompleted) != 0)
            return;
        await aggregateRecoveryGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (aggregateStagingRecoveryCompleted != 0)
                return;
            await RecoverAggregatesAsync(beforeConversion: false, cancellationToken)
                .ConfigureAwait(false);
            Volatile.Write(ref aggregateStagingRecoveryCompleted, 1);
        }
        finally
        {
            aggregateRecoveryGate.Release();
        }
    }

    private async ValueTask RecoverAggregatesAsync(
        bool beforeConversion,
        CancellationToken cancellationToken
    )
    {
        var recovery =
            new List<(ZLinkAggregateFence Fence, StoredRecord<AggregateRecord> Stored)>();
        ZLinkStoreScanCursor? cursor = null;
        var expiredRestarts = 0;
        while (true)
        {
            var scan = await _provider
                .ScanAsync(
                    new ZLinkStoreScanRequest(AggregatePrefix, cursor, 256),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                if (++expiredRestarts > 8)
                    throw new IOException(
                        "The Location Store aggregate recovery scan expired repeatedly."
                    );
                recovery.Clear();
                cursor = null;
                continue;
            }
            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            foreach (var pair in page.Items)
            {
                if (
                    pair.Key.Value.Contains(":participant:", StringComparison.Ordinal)
                    || pair.Key.Value.EndsWith(":housekeeping", StringComparison.Ordinal)
                )
                    continue;

                AggregateRecord aggregate;
                try
                {
                    aggregate = await DecodeAggregateAsync(pair.Key, pair.Value, cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (Exception error)
                    when (error
                            is JsonException
                                or InvalidDataException
                                or NotSupportedException
                                or ArgumentException
                    )
                {
                    throw new ZLinkRelocationDataLostException(
                        $"Aggregate durable root '{pair.Key.Value}' cannot be decoded ({error.GetType().Name})."
                    );
                }
                if (aggregate.Header is null || !Enum.IsDefined(aggregate.Status))
                    throw new ZLinkRelocationDataLostException(
                        $"Aggregate durable root '{pair.Key.Value}' has invalid metadata."
                    );
                var fence = new ZLinkAggregateFence(
                    aggregate.Header.AggregateId,
                    aggregate.Header.AggregateGeneration
                );
                if (
                    beforeConversion
                    && (
                        fence.AggregateId == Guid.Empty
                        || fence.AggregateGeneration == 0
                        || AggregateKey(fence) != pair.Key
                    )
                )
                    throw new ZLinkRelocationDataLostException(
                        $"Aggregate durable root '{pair.Key.Value}' has a mismatched identity."
                    );

                var terminal =
                    aggregate.Status is AggregateStatus.Committed or AggregateStatus.Aborted;
                if (
                    (beforeConversion || !terminal)
                    && (
                        (
                            !beforeConversion
                            && aggregate.StagedAt > pair.Value.StoreNow - AggregateStagingRetention
                        )
                        || await ReadLiveOwnerAsync(aggregate.Header.TargetOwner, cancellationToken)
                            .ConfigureAwait(false)
                            is not null
                    )
                )
                {
                    if (beforeConversion)
                    {
                        // Another converter can enable v12 traffic after our first marker read.
                        if (
                            await _provider
                                .ReadAsync(FormatKey, cancellationToken)
                                .ConfigureAwait(false) is ZLinkStoreReadResult.Found
                        )
                            return;
                        throw new ZLinkConfigurationException(
                            $"v11 aggregate '{pair.Key.Value}' has a live target owner."
                        );
                    }
                    continue;
                }
                recovery.Add((fence, new(aggregate, pair.Value.Version, pair.Value.StoreNow)));
            }
            cursor = page.NextCursor;
            if (cursor is null)
                break;
        }

        var withoutParticipants = new HashSet<ZLinkAggregateFence>();
        if (beforeConversion && recovery.Count != 0)
        {
            if (
                await _provider.ReadAsync(FormatKey, cancellationToken).ConfigureAwait(false)
                is ZLinkStoreReadResult.Found
            )
                return;

            // Verify the durable inputs before issuing any recovery writes.
            for (var index = recovery.Count - 1; index >= 0; index--)
            {
                var (fence, stored) = recovery[index];
                if (
                    await CanFinishLegacyWithoutParticipantsAsync(
                            fence,
                            stored.Record,
                            cancellationToken
                        )
                        .ConfigureAwait(false)
                )
                {
                    withoutParticipants.Add(fence);
                    continue;
                }
                try
                {
                    await ReadAndValidateAggregateInventoryAsync(
                            fence,
                            stored.Record,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }
                catch (ZLinkRelocationDataLostException)
                {
                    var completed = await ReadRecordAsync<AggregateRecord>(
                            AggregateKey(fence),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    if (completed is null)
                    {
                        recovery.RemoveAt(index);
                        continue;
                    }
                    if (
                        !await CanFinishLegacyTerminalAsync(
                                fence,
                                completed.Record,
                                cancellationToken
                            )
                            .ConfigureAwait(false)
                    )
                        throw;
                    recovery[index] = (fence, completed);
                    withoutParticipants.Add(fence);
                }
            }
        }

        foreach (var (fence, stored) in recovery)
        {
            try
            {
                if (withoutParticipants.Contains(fence))
                    await FinishLegacyWithoutParticipantsAsync(fence, stored, cancellationToken)
                        .ConfigureAwait(false);
                else
                    switch (stored.Record.Status)
                    {
                        case AggregateStatus.Staging:
                            await AbortAggregateStagingAsync(fence, stored, cancellationToken)
                                .ConfigureAwait(false);
                            break;
                        case AggregateStatus.Prepared:
                        case AggregateStatus.Aborted:
                            await AbortAggregateAsync(fence, beforeConversion, cancellationToken)
                                .ConfigureAwait(false);
                            break;
                        case AggregateStatus.Committed:
                            if (
                                stored.Record.ParticipantsCleaned
                                || await IsAggregateRootMissingAsync(fence, cancellationToken)
                                    .ConfigureAwait(false)
                            )
                                await RemoveSealedInventoryAsync(fence, cancellationToken)
                                    .ConfigureAwait(false);
                            else
                                await NormalizeCommittedAggregateAsync(
                                        fence,
                                        stored.Record,
                                        cancellationToken
                                    )
                                    .ConfigureAwait(false);
                            break;
                    }
            }
            catch (ZLinkRelocationDataLostException) when (beforeConversion)
            {
                var completed = await ReadRecordAsync<AggregateRecord>(
                        AggregateKey(fence),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (completed is null)
                    continue;
                if (
                    !await CanFinishLegacyTerminalAsync(fence, completed.Record, cancellationToken)
                        .ConfigureAwait(false)
                )
                    throw;
                await FinishLegacyWithoutParticipantsAsync(fence, completed, cancellationToken)
                    .ConfigureAwait(false);
            }

            if (!beforeConversion)
                continue;
            var key = AggregateKey(fence);
            var settled = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                .ConfigureAwait(false);
            if (settled is null)
                continue;
            if (
                settled.Record.Status is not (AggregateStatus.Aborted or AggregateStatus.Committed)
                || !settled.Record.ParticipantsCleaned
            )
                throw new ZLinkConfigurationException(
                    $"v11 aggregate '{fence.AggregateId:N}' could not be settled."
                );

            var removed = await WriteMigrationBatchAsync(
                    new(
                        [new ZLinkStoreCondition.Version(key, settled.Version)],
                        [
                            new ZLinkStoreMutation.Delete(key),
                            new ZLinkStoreMutation.Delete(AggregateHousekeepingKey(key)),
                        ]
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (
                !removed
                && await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false)
                    is not ZLinkStoreReadResult.Missing
            )
                throw new ZLinkConfigurationException(
                    $"v11 aggregate '{fence.AggregateId:N}' could not be removed."
                );
        }
    }

    private async ValueTask<bool> StageAggregateParticipantsAsync(
        ZLinkAggregateFence fence,
        IReadOnlyList<AggregateParticipantRecord> records,
        CancellationToken cancellationToken
    )
    {
        var exact = new bool[records.Count];
        await Parallel
            .ForEachAsync(
                records,
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (record, token) =>
                {
                    var key = AggregateParticipantMetaKey(fence, record.Index);
                    var result = await _provider
                        .WriteAsync(
                            new ZLinkStoreWriteRequest(
                                [new ZLinkStoreCondition.Missing(key)],
                                [new ZLinkStoreMutation.Put(key, Encode(record), null)]
                            ),
                            token
                        )
                        .ConfigureAwait(false);
                    if (result is ZLinkStoreWriteResult.Applied)
                    {
                        exact[record.Index] = true;
                        return;
                    }
                    var existing = await ReadRecordAsync<AggregateParticipantRecord>(key, token)
                        .ConfigureAwait(false);
                    exact[record.Index] =
                        existing is not null
                        && existing.Record.Index == record.Index
                        && existing.Record.Key == record.Key
                        && existing
                            .Record.RequestFingerprint.AsSpan()
                            .SequenceEqual(record.RequestFingerprint);
                }
            )
            .ConfigureAwait(false);
        return exact.All(static value => value);
    }

    private async ValueTask<
        IReadOnlyList<AggregateInventoryLeafEntry>
    > ReadAndValidateAggregateInventoryAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken
    )
    {
        var entries = await ZLinkAggregateInventory
            .ReadEntriesAsync(
                _provider,
                Prefix,
                fence,
                Convert.ToHexString(aggregate.Header.InventoryDigest).ToLowerInvariant(),
                cancellationToken
            )
            .ConfigureAwait(false);
        if (entries.Count != aggregate.ParticipantCount)
            throw ZLinkAggregateInventory.DataLost(ZLinkAggregateInventory.RootKey(Prefix, fence));
        return entries;
    }

    private async ValueTask<ulong[]> InstallAggregateParticipantFencesAsync(
        ZLinkAggregateFence fence,
        ZLinkAggregatePrepareRequest request,
        IReadOnlyList<StoredAuthority?> observedAuthorities,
        IReadOnlyList<GenerationCounter?> observedOwnerCounters,
        CancellationToken cancellationToken
    )
    {
        var generations = new ulong[request.Participants.Count];
        await Parallel
            .ForEachAsync(
                Enumerable.Range(0, request.Participants.Count),
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (index, token) =>
                {
                    var participant = request.Participants[index];
                    var metaKey = AggregateParticipantMetaKey(fence, index);
                    var authorityKey = AuthorityMetaKey(participant.Key);
                    for (var attempt = 0; attempt < 8; attempt++)
                    {
                        var authority =
                            attempt == 0
                                ? observedAuthorities[index]
                                : await ReadAuthorityRecordAsync(
                                        participant.Key,
                                        knownMeta: null,
                                        projectCommittedAggregate: false,
                                        token
                                    )
                                    .ConfigureAwait(false);
                        var staged = await ReadRecordAsync<AggregateParticipantRecord>(
                                metaKey,
                                token
                            )
                            .ConfigureAwait(false);
                        if (authority is null || staged is null)
                            return;
                        if (
                            authority.Meta.AggregateFence == fence
                            && authority.Meta.AggregateParticipantIndex == index
                            && string.Equals(
                                authority.Meta.AggregateExpectedStoreVersion,
                                participant.ExpectedStoreVersion,
                                StringComparison.Ordinal
                            )
                        )
                        {
                            generations[index] = staged.Record.TargetAuthorityOwnerGeneration;
                            return;
                        }
                        if (
                            authority.Meta.AggregateFence is not null
                            || !string.Equals(
                                authority.Version.Value,
                                participant.CompareExchangeStoreVersion,
                                StringComparison.Ordinal
                            )
                        )
                            return;

                        var conditions = new List<ZLinkStoreCondition>
                        {
                            new ZLinkStoreCondition.Version(authorityKey, authority.Version),
                            new ZLinkStoreCondition.Version(metaKey, staged.Version),
                        };
                        var mutations = new List<ZLinkStoreMutation>();
                        var generation = authority.Meta.AuthorityOwnerGeneration;
                        if (
                            participant.OwnerTransition
                            == ZLinkAuthorityGenerationTransition.NewOwner
                        )
                        {
                            if (observedOwnerCounters[index] is not { } counter)
                                return;
                            if (counter.Value <= generation)
                                throw new InvalidDataException(
                                    "The Location Store authority owner counter precedes its authority."
                                );
                            generation = counter.Issue(conditions, mutations);
                        }
                        mutations.Add(
                            new ZLinkStoreMutation.Put(
                                metaKey,
                                Encode(
                                    staged.Record with
                                    {
                                        TargetAuthorityOwnerGeneration = generation,
                                    }
                                ),
                                null
                            )
                        );
                        mutations.Add(
                            new ZLinkStoreMutation.Put(
                                authorityKey,
                                Encode(
                                    authority.Meta with
                                    {
                                        AggregateFence = fence,
                                        AggregateParticipantIndex = index,
                                        AggregateExpectedStoreVersion =
                                            participant.ExpectedStoreVersion,
                                    }
                                ),
                                null
                            )
                        );

                        var result = await _provider
                            .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), token)
                            .ConfigureAwait(false);
                        if (result is ZLinkStoreWriteResult.Applied)
                        {
                            generations[index] = generation;
                            return;
                        }
                    }
                }
            )
            .ConfigureAwait(false);
        return generations;
    }

    private async ValueTask<StoredAggregateParticipant[]> ReadAggregateParticipantsAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken
    )
    {
        var inventory = await ZLinkAggregateInventory
            .ReadAsync(
                _provider,
                Prefix,
                fence,
                Convert.ToHexString(aggregate.Header.InventoryDigest).ToLowerInvariant(),
                cancellationToken
            )
            .ConfigureAwait(false);
        if (inventory.Count != aggregate.ParticipantCount)
            throw ZLinkAggregateInventory.DataLost(ZLinkAggregateInventory.RootKey(Prefix, fence));
        var participants = new StoredAggregateParticipant[inventory.Count];
        await Parallel
            .ForEachAsync(
                inventory,
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (participant, token) =>
                    participants[participant.Entry.Index] = await BindAggregateParticipantAsync(
                            fence,
                            aggregate,
                            participant,
                            token
                        )
                        .ConfigureAwait(false)
            )
            .ConfigureAwait(false);
        return participants;
    }

    private async ValueTask<StoredAggregateParticipant> ReadAggregateParticipantAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        int index,
        CancellationToken cancellationToken
    )
    {
        var root = await ZLinkAggregateInventory
            .ReadRootAsync(
                _provider,
                Prefix,
                fence,
                Convert.ToHexString(aggregate.Header.InventoryDigest).ToLowerInvariant(),
                cancellationToken
            )
            .ConfigureAwait(false);
        if (root.TotalCount != aggregate.ParticipantCount || index < 0 || index >= root.TotalCount)
            throw ZLinkAggregateInventory.DataLost(ZLinkAggregateInventory.RootKey(Prefix, fence));
        var reference = root.TopPages[0];
        while (true)
        {
            var page = await ZLinkAggregateInventory
                .ReadPageAsync(_provider, Prefix, fence, reference, cancellationToken)
                .ConfigureAwait(false);
            if (page.Level == 0)
            {
                var participant = await ZLinkAggregateInventory
                    .ReadParticipantAsync(
                        _provider,
                        Prefix,
                        fence,
                        page.Entries[index - page.StartIndex],
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                return await BindAggregateParticipantAsync(
                        fence,
                        aggregate,
                        participant,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            reference = page.Children.First(child =>
                (long)child.StartIndex + child.EntryCount > index
            );
        }
    }

    private async ValueTask<StoredAggregateParticipant> BindAggregateParticipantAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        ZLinkAggregateInventory.Participant participant,
        CancellationToken cancellationToken
    )
    {
        var entry = participant.Entry;
        var key = AggregateParticipantMetaKey(fence, entry.Index);
        var meta = await ReadRecordAsync<AggregateParticipantRecord>(key, cancellationToken)
            .ConfigureAwait(false);
        if (
            meta is null
            || meta.Record.Index != entry.Index
            || meta.Record.Key != entry.AuthorityKey
            || meta.Record.ExpectedStoreVersion != entry.ExpectedStoreVersion
            || (
                meta.Record.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner
                    ? "newOwner"
                    : "preserve"
            ) != entry.OwnerTransition
            || !meta.Record.RequestFingerprint.AsSpan().SequenceEqual(aggregate.RequestFingerprint)
        )
            throw ZLinkAggregateInventory.DataLost(key);
        return new StoredAggregateParticipant(
            meta.Record,
            participant.Payload,
            participant.Mutation
        );
    }

    private async ValueTask<
        IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
    > ReadCurrentAuthorityGenerationsAsync(
        IReadOnlyList<ZLinkAggregateParticipant> participants,
        CancellationToken cancellationToken
    )
    {
        var generations = new ulong[participants.Count];
        await Parallel
            .ForEachAsync(
                Enumerable.Range(0, participants.Count),
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = MaximumStoreConcurrency,
                    CancellationToken = cancellationToken,
                },
                async (index, token) =>
                {
                    var authority =
                        await ReadAuthorityRecordAsync(participants[index].Key, token)
                            .ConfigureAwait(false)
                        ?? throw new InvalidDataException(
                            $"Aggregate participant authority '{participants[index].Key.Value}' is missing."
                        );
                    if (authority.Meta.AuthorityOwnerGeneration == 0)
                        throw new InvalidDataException(
                            $"Aggregate participant authority '{participants[index].Key.Value}' has an invalid generation."
                        );
                    generations[index] = authority.Meta.AuthorityOwnerGeneration;
                }
            )
            .ConfigureAwait(false);
        return participants
            .Select(
                (participant, index) => new { participant.Key, Generation = generations[index] }
            )
            .ToDictionary(static pair => pair.Key, static pair => pair.Generation);
    }

    private async ValueTask<ZLinkAggregatePrepareResult> ReconcileExistingAggregateAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        ZLinkAggregatePrepareRequest request,
        CancellationToken cancellationToken
    )
    {
        if (
            aggregate.Status is not (AggregateStatus.Prepared or AggregateStatus.Committed)
            || !AggregateHeaderEquals(aggregate.Header, CreateAggregateHeader(request))
            || aggregate.ParticipantCount != request.Participants.Count
        )
            return new ZLinkAggregatePrepareResult.Conflict();

        IReadOnlyDictionary<ZLinkAuthorityKey, ulong> generations;
        if (!aggregate.ParticipantsNormalized)
        {
            var participants = await ReadAggregateParticipantsAsync(
                    fence,
                    aggregate,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (!AggregateParticipantsEqual(participants, request.Participants))
                return new ZLinkAggregatePrepareResult.Conflict();
            generations = participants.ToDictionary(
                static participant => new ZLinkAuthorityKey(participant.Metadata.Key),
                static participant => participant.Metadata.TargetAuthorityOwnerGeneration
            );
        }
        else
            generations = await ReadCurrentAuthorityGenerationsAsync(
                    request.Participants,
                    cancellationToken
                )
                .ConfigureAwait(false);
        if (aggregate.Header.RepresentativeKey is { } representative)
        {
            if (request.Relocation?.Target is not { } target)
                return new ZLinkAggregatePrepareResult.Conflict();
            if (aggregate.Status == AggregateStatus.Committed)
            {
                if (aggregate.Header.Relocation?.Target != target)
                    return new ZLinkAggregatePrepareResult.Conflict();
            }
            else if (
                !await ReplacePreparedAttemptAsync(fence, representative, target, cancellationToken)
                    .ConfigureAwait(false)
            )
                return new ZLinkAggregatePrepareResult.Conflict();
        }

        return new ZLinkAggregatePrepareResult.AlreadyPrepared(fence)
        {
            TargetAuthorityOwnerGenerations = generations,
        };
    }

    private async ValueTask<bool> ReplacePreparedAttemptAsync(
        ZLinkAggregateFence fence,
        ZLinkAuthorityKey key,
        ZLinkAuthorityRelocation.TargetFence target,
        CancellationToken cancellationToken
    )
    {
        var aggregateKey = AggregateKey(fence);
        var aggregate = await ReadRecordAsync<AggregateRecord>(aggregateKey, cancellationToken)
            .ConfigureAwait(false);
        if (
            aggregate?.Record.Status != AggregateStatus.Prepared
            || aggregate.Record.Header.RepresentativeKey != key
        )
            return false;
        var authority = await ReadAuthorityRecordAsync(
                key,
                knownMeta: null,
                projectCommittedAggregate: false,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (
            authority is null
            || authority.Meta.AggregateFence != fence
            || authority.Snapshot.Relocation
                is not { Phase: ServiceWireCodec.RelocationPhase.Prepared } relocation
            || relocation.RelocationId != fence.AggregateId
            || !relocation.AcceptsAttempt(target)
        )
            return false;
        if (relocation.Target == target)
            return true;

        var authorityKey = AuthorityMetaKey(key);
        var updated = authority.Meta with
        {
            Aggregate = ZLinkAuthorityAggregateCodec.Replace(
                authority.Meta.Aggregate,
                relocation with
                {
                    Target = target,
                }
            ),
        };
        return await _provider
                .WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [
                            new ZLinkStoreCondition.Version(authorityKey, authority.Version),
                            new ZLinkStoreCondition.Version(aggregateKey, aggregate.Version),
                        ],
                        [new ZLinkStoreMutation.Put(authorityKey, Encode(updated), null)]
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false) is ZLinkStoreWriteResult.Applied;
    }

    private async ValueTask<ZLinkAggregatePrepareResult> ReconcileExistingAggregateForAmbiguousAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        ZLinkAggregatePrepareRequest request
    )
    {
        using var deadline = new CancellationTokenSource(AmbiguousReconciliationTimeout);
        return await ReconcileExistingAggregateAsync(fence, aggregate, request, deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private async ValueTask NormalizeCommittedAggregateForAmbiguousAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate
    )
    {
        using var deadline = new CancellationTokenSource(AmbiguousReconciliationTimeout);
        await NormalizeCommittedAggregateAsync(fence, aggregate, deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private static bool AggregateParticipantsEqual(
        IReadOnlyList<StoredAggregateParticipant> stored,
        IReadOnlyList<ZLinkAggregateParticipant> requested
    )
    {
        if (stored.Count != requested.Count)
            return false;
        for (var index = 0; index < stored.Count; index++)
        {
            var actual = stored[index];
            var expected = requested[index];
            if (
                !string.Equals(actual.Metadata.Key, expected.Key.Value, StringComparison.Ordinal)
                || !string.Equals(
                    actual.Metadata.ExpectedStoreVersion,
                    expected.ExpectedStoreVersion,
                    StringComparison.Ordinal
                )
                || actual.Metadata.OwnerTransition != expected.OwnerTransition
                || !CryptographicOperations.FixedTimeEquals(
                    actual.Payload.Span,
                    expected.AuthorityPayload.Span
                )
                || !actual.MembershipMutation.Span.SequenceEqual(expected.MembershipMutation.Span)
            )
                return false;
        }
        return true;
    }

    private static ZLinkAggregatePrepareRequest RehydrateAggregateRequest(
        AggregateRecord aggregate,
        IReadOnlyList<StoredAggregateParticipant> participants
    ) =>
        new(
            aggregate.Header.AggregateId,
            aggregate.Header.AggregateGeneration,
            participants
                .Select(static participant => new ZLinkAggregateParticipant(
                    new ZLinkAuthorityKey(participant.Metadata.Key),
                    participant.Metadata.ExpectedStoreVersion,
                    participant.Metadata.OwnerTransition,
                    participant.Payload,
                    participant.MembershipMutation
                ))
                .ToArray(),
            aggregate.Header.InventoryDigest,
            aggregate.Header.TargetDescriptor,
            aggregate.Header.TargetDescriptorLifecycleGeneration,
            aggregate.Header.Capacity,
            aggregate.Header.TargetOwner
        )
        {
            Relocation = aggregate.Header.Relocation,
            RepresentativeKey = aggregate.Header.RepresentativeKey,
            ClearRelocation = aggregate.Header.ClearRelocation,
        };

    private static AggregateHeader CreateAggregateHeader(ZLinkAggregatePrepareRequest request) =>
        new(
            request.AggregateId,
            request.AggregateGeneration,
            request.InventoryDigest.ToArray(),
            request.TargetDescriptor,
            request.TargetDescriptorLifecycleGeneration,
            request.Capacity,
            request.TargetOwner
        )
        {
            Relocation = request.RepresentativeKey is null ? request.Relocation : null,
            RepresentativeKey = request.RepresentativeKey,
            ClearRelocation = request.ClearRelocation,
        };

    private static bool AggregateHeaderEquals(AggregateHeader left, AggregateHeader right) =>
        left with
        {
            InventoryDigest = right.InventoryDigest,
            Relocation = left.RepresentativeKey is not null ? right.Relocation : left.Relocation,
        } == right
        && left.InventoryDigest.AsSpan().SequenceEqual(right.InventoryDigest);

    private static byte[] AggregateRequestFingerprint(AggregateHeader header, int count) =>
        SHA256.HashData(
            ZLinkAggregateInventory.Encode(AggregateBody(header, count, AggregateStatus.Staging))
        );

    private static ZLinkAggregatePrepareRequest CanonicalizeAggregateRequest(
        ZLinkAggregatePrepareRequest request
    )
    {
        var participants = request
            .Participants.OrderBy(
                static participant => participant.Key.Value,
                StringComparer.Ordinal
            )
            .Select(static participant =>
                participant.MembershipMutation.IsEmpty
                    ? participant with
                    {
                        MembershipMutation = ZLinkAggregateInventory.NoMembershipMutation,
                    }
                    : participant
            )
            .ToArray();
        return request with { Participants = participants };
    }

    private async ValueTask<ZLinkAggregatePrepareResult> ReconcileAggregatePrepareAsync(
        ZLinkStoreKey key,
        ZLinkAggregateFence fence,
        ZLinkAggregatePrepareRequest request,
        CancellationToken cancellationToken
    )
    {
        var stored = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
            .ConfigureAwait(false);
        return stored is null
            ? new ZLinkAggregatePrepareResult.Conflict()
            : await ReconcileExistingAggregateAsync(
                    fence,
                    stored.Record,
                    request,
                    cancellationToken
                )
                .ConfigureAwait(false);
    }

    public ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default
    ) => AbortAggregateAsync(fence, beforeConversion: false, cancellationToken);

    public ValueTask<ZLinkAggregateAbortResult> AbortAggregateAttemptAsync(
        ZLinkAggregateFence fence,
        ulong attempt,
        CancellationToken cancellationToken
    ) => AbortAggregateAsync(fence, beforeConversion: false, cancellationToken, attempt);

    private async ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
        ZLinkAggregateFence fence,
        bool beforeConversion,
        CancellationToken cancellationToken,
        ulong? attempt = null
    )
    {
        var key = AggregateKey(fence);
        while (true)
        {
            var aggregate = await ReadRecordAsync<AggregateRecord>(key, cancellationToken)
                .ConfigureAwait(false);
            if (aggregate is null)
                return ZLinkAggregateAbortResult.Stale;
            if (aggregate.Record.Status == AggregateStatus.Aborted)
            {
                if (
                    !aggregate.Record.ParticipantsCleaned
                    && !await IsAggregateRootMissingAsync(fence, cancellationToken)
                        .ConfigureAwait(false)
                )
                    await ReleaseAggregateParticipantFencesAsync(
                            fence,
                            aggregate.Record,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                await CleanupAggregateParticipantsAsync(fence, cancellationToken)
                    .ConfigureAwait(false);
                return ZLinkAggregateAbortResult.AlreadyAborted;
            }
            if (aggregate.Record.Status == AggregateStatus.Staging)
                return await AbortAggregateStagingAsync(fence, aggregate, cancellationToken)
                    .ConfigureAwait(false)
                    ? ZLinkAggregateAbortResult.Aborted
                    : ZLinkAggregateAbortResult.Stale;
            if (aggregate.Record.Status == AggregateStatus.Committed)
                return ZLinkAggregateAbortResult.Stale;
            // Abort needs the target capacity vector from the durable aggregate
            // header. Participant meta is not authoritative for fence release and
            // may already be missing after a partial cleanup.
            var inventory = await ReadAndValidateAggregateInventoryAsync(
                    fence,
                    aggregate.Record,
                    cancellationToken
                )
                .ConfigureAwait(false);
            var header = aggregate.Record.Header;
            var conditions = new List<ZLinkStoreCondition>
            {
                new ZLinkStoreCondition.Version(key, aggregate.Version),
            };
            if (attempt is not null && header.RepresentativeKey is { } representative)
            {
                var authority = await ReadAuthorityRecordAsync(
                        representative,
                        knownMeta: null,
                        projectCommittedAggregate: false,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (
                    authority?.Meta.AggregateFence != fence
                    || authority.Snapshot.Relocation?.Target?.AttemptGeneration != attempt
                )
                    return ZLinkAggregateAbortResult.Stale;
                conditions.Add(
                    new ZLinkStoreCondition.Version(
                        AuthorityMetaKey(representative),
                        authority.Version
                    )
                );
            }
            var mutations = new List<ZLinkStoreMutation>
            {
                new ZLinkStoreMutation.Put(
                    key,
                    Encode(aggregate.Record with { Status = AggregateStatus.Aborted }),
                    null
                ),
            };
            if (beforeConversion)
            {
                try
                {
                    await AppendLegacyCapacityReleaseAsync(
                            header,
                            conditions,
                            mutations,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }
                catch (ZLinkConfigurationException)
                {
                    if (await MatchesAsync(conditions[0], cancellationToken).ConfigureAwait(false))
                        throw;
                    continue;
                }
            }
            else
            {
                await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
                AddCapacityMutations(
                    mutations,
                    header.TargetDescriptor,
                    header.Capacity,
                    pending: -1
                );
            }
            var result = await _provider
                .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), cancellationToken)
                .ConfigureAwait(false);
            if (result is not ZLinkStoreWriteResult.Applied)
            {
                if (beforeConversion && result is ZLinkStoreWriteResult.Conflict)
                {
                    var changed = false;
                    foreach (var condition in conditions)
                        changed |= !await MatchesAsync(condition, cancellationToken)
                            .ConfigureAwait(false);
                    if (changed)
                        continue;
                }
                return ZLinkAggregateAbortResult.Stale;
            }
            await ReleaseAggregateParticipantFencesAsync(
                    fence,
                    aggregate.Record,
                    cancellationToken,
                    inventory
                )
                .ConfigureAwait(false);
            await CleanupAggregateParticipantsAsync(fence, cancellationToken).ConfigureAwait(false);
            return ZLinkAggregateAbortResult.Aborted;
        }
    }

    private async ValueTask<ZLinkObjectCommitResult> CompleteCommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        DateTimeOffset? operationDeadline,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        ArgumentNullException.ThrowIfNull(reservation);
        ValidateAuthorityPayload(readyPayload);
        var counterRetry = 0;
        var retryDeadline = operationDeadline is null
            ? Stopwatch.GetElapsedTime(0) + WriteConflictRetryWindow
            : default;
        for (; ; )
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (operationDeadline is { } deadline && _time.GetUtcNow() >= deadline)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "Authority Ready commit exceeded the operation deadline."
                );
            var current = await ReadAuthorityRecordAsync(reservation.Key, cancellationToken)
                .ConfigureAwait(false);
            if (current is null)
                return new ZLinkObjectCommitResult.Stale();
            if (!MatchesReservation(current, reservation))
            {
                if (current.Meta.ReservedCreation is null && AuthorityObservedAsync is { } observed)
                    await observed(reservation.Key, current.Snapshot.StoreVersion)
                        .ConfigureAwait(false);
                return current.Meta.ReservedCreation is null
                    ? new ZLinkObjectCommitResult.AlreadyCommitted(current.Snapshot)
                    : new ZLinkObjectCommitResult.Stale();
            }
            var target = await ReadEligibleTargetAsync(
                    reservation.TargetDescriptor,
                    reservation.TargetNodeLifecycleGeneration,
                    reservation.TargetOwner,
                    current!.Snapshot.Allocation.ObjectKind,
                    current.Snapshot.Allocation.StableType,
                    cancellationToken,
                    requireNewPlacementEligibility: false
                )
                .ConfigureAwait(false);
            if (target is null)
                return new ZLinkObjectCommitResult.Stale();
            await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
            var meta = current.Meta with
            {
                Payload = readyPayload.ToArray(),
                Allocation = current.Snapshot.Allocation with
                {
                    State = ZLinkPlacementAllocationState.Active,
                },
                ReservedCreation = null,
            };
            var conditions = new List<ZLinkStoreCondition>
            {
                new ZLinkStoreCondition.Version(AuthorityMetaKey(reservation.Key), current.Version),
                target.DescriptorCondition,
                target.OwnerCondition,
            };
            if (
                (
                    await AddMembershipCommitConditionsAsync(
                            readyPayload,
                            current.Snapshot,
                            conditions,
                            cancellationToken
                        )
                        .ConfigureAwait(false)
                )
                is not null
            )
                return new ZLinkObjectCommitResult.Stale();
            var mutations = new List<ZLinkStoreMutation>
            {
                new ZLinkStoreMutation.Put(AuthorityMetaKey(reservation.Key), Encode(meta), null),
            };
            AddCapacityMutations(mutations, current.Snapshot.Allocation, pending: -1, active: 1);
            var result = await _provider
                .WriteAsync(new ZLinkStoreWriteRequest(conditions, mutations), cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Conflict)
            {
                if (operationDeadline is null)
                {
                    if (
                        counterRetry >= WriteConflictRetryLimit
                        || Stopwatch.GetElapsedTime(0) >= retryDeadline
                    )
                        return new ZLinkObjectCommitResult.Stale();
                    await DelayWriteConflictRetryAsync(
                            counterRetry,
                            retryDeadline,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    counterRetry++;
                }
                continue;
            }
            var applied = (ZLinkStoreWriteResult.Applied)result;
            if (AuthorityObservedAsync is { } committedObserver)
                await committedObserver(
                        reservation.Key,
                        applied.PutVersions[AuthorityMetaKey(reservation.Key)].Value
                    )
                    .ConfigureAwait(false);
            return new ZLinkObjectCommitResult.Committed(
                Snapshot(
                    meta,
                    applied.PutVersions[AuthorityMetaKey(reservation.Key)],
                    applied.StoreNow,
                    readyPayload
                )
            );
        }
    }

    private async ValueTask<StoredAuthority?> ReadAuthorityRecordAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken
    ) =>
        await ReadAuthorityRecordAsync(
                key,
                null,
                projectCommittedAggregate: true,
                cancellationToken
            )
            .ConfigureAwait(false);

    private async ValueTask<StoredAuthority?> ReadAuthorityRecordAsync(
        ZLinkAuthorityKey key,
        ZLinkStoreValue? knownMeta,
        bool projectCommittedAggregate,
        CancellationToken cancellationToken
    )
    {
        const int MaximumProjectionReadAttempts = 8;
        var metaKey = AuthorityMetaKey(key);

        // The authority record is one opaque row (checklist C-2b): a single
        // read is already byte-atomic, so the old read-verify-retry loop
        // that reconciled a separately read payload key against this meta
        // key's version is gone. Retry here exists only for the aggregate
        // participant-projection race noted below, not for payload/meta
        // consistency.
        for (var attempt = 0; attempt < MaximumProjectionReadAttempts; attempt++)
        {
            var first = knownMeta is null
                ? await _provider.ReadAsync(metaKey, cancellationToken).ConfigureAwait(false)
                : new ZLinkStoreReadResult.Found(knownMeta);
            knownMeta = null;
            if (first is ZLinkStoreReadResult.Missing)
                return null;
            var found = ((ZLinkStoreReadResult.Found)first).Value;
            var meta = DecodeAuthorityMeta(found.Bytes);
            ReadOnlyMemory<byte> payloadBytes = meta.Payload;
            if (projectCommittedAggregate && meta.AggregateFence is { } aggregateFence)
            {
                var aggregate = await ReadRecordAsync<AggregateRecord>(
                        AggregateKey(aggregateFence),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (aggregate is null)
                {
                    var currentMeta = await _provider
                        .ReadAsync(metaKey, cancellationToken)
                        .ConfigureAwait(false);
                    if (
                        currentMeta is not ZLinkStoreReadResult.Found current
                        || current.Value.Version != found.Version
                    )
                        continue;
                    throw ZLinkAggregateInventory.DataLost(AggregateKey(aggregateFence));
                }
                if (aggregate.Record.Status == AggregateStatus.Committed)
                {
                    if (meta.AggregateParticipantIndex is not { } index)
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate '{aggregateFence.AggregateId:N}' "
                                + $"has no participant index for authority '{key.Value}'."
                        );
                    StoredAggregateParticipant participant;
                    try
                    {
                        participant = await ReadAggregateParticipantAsync(
                                aggregateFence,
                                aggregate.Record,
                                index,
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                    }
                    catch (ZLinkRelocationDataLostException)
                    {
                        // Aggregate normalization changes the authority meta
                        // before it removes the now-unreferenced participant
                        // rows. A reader that observed the prior meta may race
                        // with that cleanup. Retry only when the authority meta
                        // version proves that a concurrent normalization won;
                        // otherwise the missing participant is real data loss.
                        var currentMeta = await _provider
                            .ReadAsync(metaKey, cancellationToken)
                            .ConfigureAwait(false);
                        if (
                            currentMeta is not ZLinkStoreReadResult.Found current
                            || current.Value.Version != found.Version
                        )
                            continue;
                        throw;
                    }
                    if (
                        !string.Equals(
                            participant.Metadata.Key,
                            key.Value,
                            StringComparison.Ordinal
                        )
                    )
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate '{aggregateFence.AggregateId:N}' "
                                + $"does not contain authority '{key.Value}'."
                        );
                    var targetAuthorityOwnerGeneration = participant
                        .Metadata
                        .TargetAuthorityOwnerGeneration;
                    if (targetAuthorityOwnerGeneration == 0)
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate '{aggregateFence.AggregateId:N}' "
                                + $"has no target generation for authority '{key.Value}'."
                        );
                    var changesOwner =
                        participant.Metadata.OwnerTransition
                        == ZLinkAuthorityGenerationTransition.NewOwner;
                    payloadBytes = aggregate.Record.Header.ProjectPayload(
                        meta.Allocation.ObjectKind,
                        participant.Payload
                    );
                    meta = meta with
                    {
                        Payload = payloadBytes.ToArray(),
                        Aggregate = aggregate.Record.Header.ApplyRelocation(meta.Aggregate),
                        AuthorityOwnerGeneration = changesOwner
                            ? targetAuthorityOwnerGeneration
                            : meta.AuthorityOwnerGeneration,
                        OwnerId = changesOwner
                            ? aggregate.Record.Header.TargetOwner.OwnerId
                            : meta.OwnerId,
                        OwnerLeaseGeneration = changesOwner
                            ? aggregate.Record.Header.TargetOwner.LeaseGeneration
                            : meta.OwnerLeaseGeneration,
                        Allocation = changesOwner
                            ? meta.Allocation with
                            {
                                Descriptor = aggregate.Record.Header.TargetDescriptor,
                                DescriptorLifecycleGeneration = aggregate
                                    .Record
                                    .Header
                                    .TargetDescriptorLifecycleGeneration,
                            }
                            : meta.Allocation,
                    };
                }
            }
            return new StoredAuthority(
                key,
                meta,
                found.Version,
                Snapshot(meta, found.Version, found.StoreNow, payloadBytes)
            );
        }
        throw new IOException($"Authority '{key.Value}' changed continuously while it was read.");
    }

    private async ValueTask<StoredTarget?> ReadEligibleTargetAsync(
        ZLinkMeshNodeDescriptorKey key,
        ulong lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind? objectKind,
        string? stableType,
        CancellationToken cancellationToken,
        bool requireNewPlacementEligibility = true,
        bool allowPreparingTarget = false
    )
    {
        var descriptorKey = MeshKey(key.MeshName, key.Rid);
        var descriptorRead = await _provider
            .ReadAsync(descriptorKey, cancellationToken)
            .ConfigureAwait(false);
        if (descriptorRead is not ZLinkStoreReadResult.Found descriptorFound)
        {
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"eligible_target_rejected reason=no_descriptor mesh={key.MeshName}"
            );
            return null;
        }
        var descriptor = DecodeDescriptor<ZLinkMeshNodeDescriptor>(
            descriptorFound.Value.Bytes
        ).Descriptor;
        var ownerRead = await ReadLiveOwnerAsync(owner, cancellationToken).ConfigureAwait(false);
        //  Nine guards below share one bare null. Name the values they compare
        //  so a rejection says which one disagreed.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"eligible_target_check rid={key.Rid} owner_live={ownerRead is not null} lifecycle={descriptor.LifecycleGeneration}/{lifecycleGeneration} owner={descriptor.OwnerId}/{owner.OwnerId} lease={descriptor.LeaseGeneration}/{owner.LeaseGeneration} role={descriptor.ObjectRole} state={descriptor.State} weight={descriptor.PlacementWeight} kind={objectKind} stable_type={stableType}"
        );
        if (
            ownerRead is null
            || descriptor.MeshName != key.MeshName
            || descriptor.Rid != key.Rid
            || descriptor.LifecycleGeneration != lifecycleGeneration
            || descriptor.OwnerId != owner.OwnerId
            || descriptor.LeaseGeneration != owner.LeaseGeneration
            || descriptor.ObjectRole != ZLinkMeshNodeObjectRole.Server
            || (
                requireNewPlacementEligibility
                && (
                    descriptor.State != ZLinkFrameworkRuntimeState.Serving
                        && !(
                            allowPreparingTarget
                            && descriptor.State == ZLinkFrameworkRuntimeState.Preparing
                        )
                    || descriptor.PlacementWeight <= 0
                )
            )
        )
            return null;
        if (
            objectKind is { } kind
            && !descriptor.ObjectCapabilities.Any(capability =>
                capability.ObjectKind == kind
                && string.Equals(capability.StableType, stableType, StringComparison.Ordinal)
            )
        )
            return null;
        return new StoredTarget(
            descriptor,
            new ZLinkStoreCondition.Version(descriptorKey, descriptorFound.Value.Version),
            new ZLinkStoreCondition.Value(OwnerKey(owner.OwnerId), ownerRead.Value)
        );
    }

    private async ValueTask<StoredOwner?> ReadLiveOwnerAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken
    )
    {
        var read = await _provider
            .ReadAsync(OwnerKey(token.OwnerId), cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkStoreReadResult.Found found)
            return null;
        var record = DecodeOwner(found.Value.Bytes);
        return record.OwnerId == token.OwnerId && record.LeaseGeneration == token.LeaseGeneration
            ? new StoredOwner(token, Encode(new OwnerRecord(token.OwnerId, token.LeaseGeneration)))
            : null;
    }

    private async ValueTask<StoredTerminal?> ReadTerminalAsync(
        ZLinkCreationOperationId operation,
        CancellationToken cancellationToken
    )
    {
        var key = TerminalKey(operation);
        var read = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
        if (read is not ZLinkStoreReadResult.Found found)
            return null;
        if (found.Value.ExpiresAt is not { } expiresAt)
            throw new InvalidDataException("The creation terminal store value has no expiration.");
        var envelope = found.Value.Bytes.ToArray();
        return new StoredTerminal(
            new ZLinkCreationTerminalRecord(operation, envelope, expiresAt, found.Value.StoreNow),
            found.Value.Version
        );
    }

    private static async ValueTask DelayWriteConflictRetryAsync(
        int retry,
        TimeSpan deadline,
        CancellationToken cancellationToken
    )
    {
        var remaining = deadline - Stopwatch.GetElapsedTime(0);
        if (remaining <= TimeSpan.Zero)
            return;
        var exponentialMilliseconds = Math.Min(100, 2 << Math.Min(retry, 5));
        var delay = TimeSpan.FromMilliseconds(
            Math.Min(
                remaining.TotalMilliseconds,
                exponentialMilliseconds + Random.Shared.Next(0, exponentialMilliseconds + 1)
            )
        );
        if (delay > TimeSpan.Zero)
            await Task.Delay(delay, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<StoredRecord<T>?> ReadRecordAsync<T>(
        ZLinkStoreKey key,
        CancellationToken cancellationToken
    )
    {
        var read = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
        if (read is not ZLinkStoreReadResult.Found found)
            return null;
        try
        {
            return new StoredRecord<T>(
                typeof(T) == typeof(AggregateRecord)
                    ? (T)
                        (object)
                            await DecodeAggregateAsync(key, found.Value, cancellationToken)
                                .ConfigureAwait(false)
                    : Decode<T>(found.Value.Bytes),
                found.Value.Version,
                found.Value.StoreNow
            );
        }
        catch (Exception error)
            when (IsAggregateDurableRecord<T>()
                && error
                    is JsonException
                        or InvalidDataException
                        or FormatException
                        or NotSupportedException
                        or ArgumentException
            )
        {
            throw new ZLinkRelocationDataLostException(
                $"Aggregate durable record '{key.Value}' cannot be decoded ({error.GetType().Name})."
            );
        }
    }

    private static ZLinkStoreKey AggregateHousekeepingKey(ZLinkStoreKey key) =>
        Key(key.Value + ":housekeeping");

    private static ZLinkAggregateInventory.Aggregate AggregateBody(
        AggregateHeader header,
        int count,
        AggregateStatus status
    ) =>
        ZLinkAggregateInventory.Describe(
            new ZLinkAggregateFence(header.AggregateId, header.AggregateGeneration),
            status switch
            {
                AggregateStatus.Staging => "preparing",
                AggregateStatus.Prepared => "prepared",
                AggregateStatus.Committed => "committed",
                AggregateStatus.Aborted => "aborted",
                _ => throw new ArgumentOutOfRangeException(nameof(status)),
            },
            count,
            Convert.ToHexString(header.InventoryDigest).ToLowerInvariant(),
            header.TargetDescriptor,
            header.TargetDescriptorLifecycleGeneration,
            header.Capacity,
            header.TargetOwner
        );

    private static byte[] Encode(AggregateRecord aggregate) =>
        ZLinkAggregateInventory.Encode(
            AggregateBody(aggregate.Header, aggregate.ParticipantCount, aggregate.Status)
        );

    private async ValueTask<AggregateRecord> DecodeAggregateAsync(
        ZLinkStoreKey key,
        ZLinkStoreValue value,
        CancellationToken cancellationToken
    )
    {
        var body = ZLinkAggregateInventory.Decode<ZLinkAggregateInventory.Aggregate>(value.Bytes);
        if (
            body.Capacity is null
            || body.TargetDescriptor is null
            || body.TargetOwner is null
            || !Guid.TryParseExact(body.AggregateId, "D", out var id)
            || !ulong.TryParse(
                body.AggregateGeneration,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out var generation
            )
            || !ulong.TryParse(
                body.TargetDescriptorLifecycleGeneration,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out var lifecycle
            )
            || !long.TryParse(
                body.TargetOwner.OwnerLeaseGeneration,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out var ownerLease
            )
            || generation is 0 or > long.MaxValue
            || lifecycle == 0
            || ownerLease <= 0
            || string.IsNullOrEmpty(body.TargetOwner.OwnerId)
            || string.IsNullOrEmpty(body.TargetDescriptor.MeshName)
            || body.ParticipantCount < 0
            || body.Capacity.Actors < 0
            || body.Capacity.Spots < 0
            || body.InventoryDigestSha256 is not { Length: 64 }
        )
            throw ZLinkAggregateInventory.DataLost(key);
        var status = body.State switch
        {
            "preparing" => AggregateStatus.Staging,
            "prepared" => AggregateStatus.Prepared,
            "committed" => AggregateStatus.Committed,
            "aborted" => AggregateStatus.Aborted,
            _ => throw ZLinkAggregateInventory.DataLost(key),
        };
        var header = new AggregateHeader(
            id,
            generation,
            Convert.FromHexString(body.InventoryDigestSha256),
            new ZLinkMeshNodeDescriptorKey(
                body.TargetDescriptor.MeshName,
                RoutingId.FromHex(body.TargetDescriptor.RoutingIdHex)
            ),
            lifecycle,
            new ZLinkCapacityVector(
                body.Capacity.Actors,
                body.Capacity.Spots,
                body.Capacity.SpotType is { } spot
                    ? new ZLinkSpotTypeCapacityDelta(
                        spot.ObjectKind switch
                        {
                            "userSpot" => ZLinkPlacementObjectKind.UserSpot,
                            "instanceSpot" => ZLinkPlacementObjectKind.InstanceSpot,
                            _ => throw ZLinkAggregateInventory.DataLost(key),
                        },
                        spot.StableType,
                        spot.Count
                    )
                    : null
            ),
            new ZLinkLocationOwnerToken(body.TargetOwner.OwnerId, ownerLease)
        );
        if (
            key != AggregateKey(new ZLinkAggregateFence(id, generation))
            || body != AggregateBody(header, body.ParticipantCount, status)
        )
            throw ZLinkAggregateInventory.DataLost(key);
        var housekeeping = (
            await ReadRecordAsync<AggregateHousekeeping>(
                    AggregateHousekeepingKey(key),
                    cancellationToken
                )
                .ConfigureAwait(false)
        )?.Record;
        header = header with
        {
            Relocation = housekeeping?.Relocation,
            RepresentativeKey = housekeeping?.RepresentativeKey,
            ClearRelocation = housekeeping?.ClearRelocation ?? false,
        };
        return new AggregateRecord(
            header,
            body.ParticipantCount,
            AggregateRequestFingerprint(header, body.ParticipantCount),
            status,
            housekeeping?.StagedAt ?? value.StoreNow,
            housekeeping?.ParticipantsNormalized ?? false,
            housekeeping?.ParticipantsCleaned ?? false
        );
    }

    private static byte[] EncodeAggregateHousekeeping(AggregateRecord aggregate) =>
        Encode(
            new AggregateHousekeeping(
                aggregate.StagedAt,
                aggregate.ParticipantsNormalized,
                aggregate.ParticipantsCleaned,
                aggregate.Header.Relocation,
                aggregate.Header.ClearRelocation,
                aggregate.Header.RepresentativeKey
            )
        );

    private sealed record AggregateHousekeeping(
        DateTimeOffset StagedAt,
        bool ParticipantsNormalized,
        bool ParticipantsCleaned,
        ZLinkAuthorityRelocation? Relocation,
        bool ClearRelocation,
        ZLinkAuthorityKey? RepresentativeKey = null
    );

    private static bool IsAggregateDurableRecord<T>() =>
        typeof(T) == typeof(AggregateRecord) || typeof(T) == typeof(AggregateParticipantRecord);

    private async ValueTask<StoredRecord<T>?> ReadRecordForReconciliationAsync<T>(ZLinkStoreKey key)
    {
        using var deadline = new CancellationTokenSource(AmbiguousReconciliationTimeout);
        return await ReadRecordAsync<T>(key, deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private async ValueTask<GenerationCounter> ReadGenerationCounterAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken
    )
    {
        await EnsureStoreFormatAsync(cancellationToken).ConfigureAwait(false);
        var read = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
        return read switch
        {
            ZLinkStoreReadResult.Missing => new GenerationCounter(
                key,
                Volatile.Read(ref _generationFloor),
                new ZLinkStoreCondition.Missing(key)
            ),
            ZLinkStoreReadResult.Found found => new GenerationCounter(
                key,
                DecodeGenerationCounter(found.Value.Bytes),
                new ZLinkStoreCondition.Version(key, found.Value.Version)
            ),
            _ => throw new InvalidOperationException(),
        };
    }

    private async ValueTask<DateTimeOffset> ReadStoreNowAsync(CancellationToken cancellationToken)
    {
        var read = await _provider
            .ReadAsync(Key(Prefix + "clock"), cancellationToken)
            .ConfigureAwait(false);
        return read switch
        {
            ZLinkStoreReadResult.Missing missing => missing.StoreNow,
            ZLinkStoreReadResult.Found found => found.Value.StoreNow,
            _ => throw new InvalidOperationException(),
        };
    }

    private static bool MatchesReservation(
        StoredAuthority? current,
        ZLinkObjectReservation reservation
    ) =>
        current is not null
        && current.Version.Value == reservation.StoreVersion
        && current.Meta.ReservedCreation is { } pendingCreation
        && pendingCreation.ReservationId == reservation.ReservationVersion
        && current.Snapshot.ObjectGeneration == reservation.ObjectGeneration
        && current.Snapshot.AuthorityOwnerGeneration == reservation.AuthorityOwnerGeneration
        && current.Snapshot.Allocation.Descriptor == reservation.TargetDescriptor
        && current.Snapshot.Allocation.DescriptorLifecycleGeneration
            == reservation.TargetNodeLifecycleGeneration
        && current.Snapshot.OwnerId == reservation.TargetOwner.OwnerId
        && current.Snapshot.OwnerLeaseGeneration == reservation.TargetOwner.LeaseGeneration
        && current.Snapshot.Allocation.State == ZLinkPlacementAllocationState.Reserved;

    private static ZLinkAuthorityCompareExchangeResult.Conflict Conflict(
        StoredAuthority? current
    ) =>
        new ZLinkAuthorityCompareExchangeResult.Conflict(
            current is null
                ? new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow)
                : new ZLinkAuthorityReadResult.Found(current.Snapshot)
        );

    private static JsonElement? ApplyAuthorityRelocation(
        StoredAuthority current,
        ZLinkAuthorityMutation mutation
    ) =>
        mutation.Relocation is not null || mutation.ClearRelocation
            ? ZLinkAuthorityAggregateCodec.Replace(
                current.Meta.Aggregate,
                mutation.ApplyRelocation(current.Snapshot)
            )
            : current.Meta.Aggregate;

    private static ZLinkAuthoritySnapshot Snapshot(
        AuthorityMeta meta,
        ZLinkStoreVersion version,
        DateTimeOffset now,
        ReadOnlyMemory<byte> payload
    ) =>
        new(
            version.Value,
            payload.ToArray(),
            meta.ObjectGeneration,
            meta.AuthorityOwnerGeneration,
            meta.OwnerId,
            meta.OwnerLeaseGeneration,
            meta.Allocation,
            meta.ReservedCreation,
            now
        )
        {
            AggregateExpectedStoreVersion = meta.AggregateExpectedStoreVersion,
            Relocation = meta.Aggregate is { } aggregate
                ? ZLinkAuthorityAggregateCodec.Read(aggregate)
                : null,
        };

    private static bool IsEligible(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkPlacementAllocation allocation
    ) =>
        descriptor.ObjectCapabilities.Any(capability =>
            capability.ObjectKind == allocation.ObjectKind
            && capability.StableType == allocation.StableType
        );

    private static void AddCondition(
        ICollection<ZLinkStoreCondition> conditions,
        ZLinkStoreCondition condition
    )
    {
        var key = condition switch
        {
            ZLinkStoreCondition.Missing value => value.Key,
            ZLinkStoreCondition.Version value => value.Key,
            ZLinkStoreCondition.Value value => value.Key,
            _ => throw new ArgumentOutOfRangeException(nameof(condition)),
        };
        if (
            conditions.Any(existing =>
                existing switch
                {
                    ZLinkStoreCondition.Missing value => value.Key == key,
                    ZLinkStoreCondition.Version value => value.Key == key,
                    ZLinkStoreCondition.Value value => value.Key == key,
                    _ => false,
                }
            )
        )
            return;
        conditions.Add(condition);
    }

    private static void ValidateAuthorityKey(ZLinkAuthorityKey key) =>
        ArgumentException.ThrowIfNullOrWhiteSpace(key.Value);

    private static void ValidateAuthorityPayload(ReadOnlyMemory<byte> payload)
    {
        if (payload.Length > 1024 * 1024)
            throw new ArgumentOutOfRangeException(nameof(payload));
    }

    private static void ValidateReservation(ZLinkObjectReservationRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateAuthorityKey(request.Key);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.StableType);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.CreationIntentReference);
        if (
            request.CreationIntentHash.Length != 32
            || request.CreationIntentEncodedSize is < 0 or > 1024 * 1024
            || request.CreatingPayload.Length > 1024 * 1024
            || request.TargetNodeLifecycleGeneration == 0
            || request.TargetOwner.LeaseGeneration <= 0
        )
            throw new ArgumentOutOfRangeException(nameof(request));
    }

    private static void ValidateAggregate(ZLinkAggregatePrepareRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (
            request.AggregateId == Guid.Empty
            || request.AggregateGeneration is 0 or > long.MaxValue
            || request.Participants.Count < 1
            || request.InventoryDigest.Length != 32
            || request.TargetDescriptorLifecycleGeneration == 0
            || request.TargetOwner.LeaseGeneration <= 0
            || request
                .Participants.Select(value => value.Key.Value)
                .Distinct(StringComparer.Ordinal)
                .Count() != request.Participants.Count
        )
            throw new ArgumentOutOfRangeException(nameof(request));
        if (
            request.Participants.Any(participant =>
                string.IsNullOrWhiteSpace(participant.Key.Value)
                || string.IsNullOrWhiteSpace(participant.ExpectedStoreVersion)
                || participant.AuthorityPayload.Length > 1024 * 1024
                || participant.OwnerTransition
                    is not (
                        ZLinkAuthorityGenerationTransition.Preserve
                        or ZLinkAuthorityGenerationTransition.NewOwner
                    )
            )
        )
            throw new ArgumentOutOfRangeException(nameof(request));
    }

    private static void ValidateTerminal(ZLinkCreationTerminalPublication publication)
    {
        ValidateCreationOperation(publication.Operation);
        if (publication.TerminalEnvelope.Length > 1024 * 1024)
            throw new ArgumentException(
                "The creation terminal publication is invalid.",
                nameof(publication)
            );
    }

    private static void ValidateCreationOperation(ZLinkCreationOperationId operation)
    {
        if (
            operation.SourceNodeRid.IsEmpty
            || operation.SourceNodeGeneration == 0
            || operation.OperationIdHigh == 0 && operation.OperationIdLow == 0
        )
            throw new ArgumentOutOfRangeException(nameof(operation));
    }

    private static byte[] Sha256(ReadOnlyMemory<byte> payload) => SHA256.HashData(payload.Span);

    private static byte[] Encode<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, ZLinkJsonSerializerOptions.Default);

    private static byte[] EncodeGenerationCounter(ulong value) =>
        Encoding.UTF8.GetBytes(value.ToString(CultureInfo.InvariantCulture));

    private static ulong DecodeGenerationCounter(ReadOnlyMemory<byte> bytes)
    {
        var text = Encoding.UTF8.GetString(bytes.Span);
        if (
            !ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var value)
            || value is 0 or > MaximumGeneration
            || !string.Equals(
                text,
                value.ToString(CultureInfo.InvariantCulture),
                StringComparison.Ordinal
            )
        )
        {
            throw new InvalidDataException(
                "The Location Store generation counter is not canonical decimal."
            );
        }

        return value;
    }

    // Authority-record reader (spec 21 §420-425: every opaque record reader
    // MUST fail on an unrecognized recordVersion instead of guessing how to
    // read it). Same fail-closed shape as DecodeDescriptor/DecodeOwner.
    private static AuthorityMeta DecodeAuthorityMeta(ReadOnlyMemory<byte> bytes)
    {
        var meta = Decode<AuthorityMeta>(bytes);
        if (meta.RecordVersion != 1)
            throw new InvalidDataException(
                "The Location Store authority record has an unrecognized" + " recordVersion."
            );
        return meta;
    }

    // Checklist C-2b / 21-location-runtime.md#2.4: the authority record is
    // one logical opaque row addressed by one key. cpp/java/node already
    // store the whole authority this way; the separate payload and
    // generation-counter keys this file used to write alongside
    // AuthorityMetaKey are gone -- AuthorityMeta now carries the payload
    // bytes and both generation values directly, so a single Put/Delete on
    // this key is already byte-atomic and needs no split-read integrity
    // check.
    //
    // Checklist C-4e: the Redis key for the authority record is the same
    // public, cross-language contract as Mesh/Owner/ClientServer/FanoutKey
    // above (21-location-runtime.md#2.4) -- the provider hashes the key
    // value with SHA-256, so the value must equal the exact NUL-delimited
    // logical key preimage the spec pins: "authority\0{actor|spot}\0{Id}".
    // ZLinkAuthorityKey.Value itself stays the "zla1:..." percent-encoded
    // contract value used everywhere else in this codebase as the
    // in-process identity handle (equality, dictionary keys, wire framing);
    // this method and DecodeAuthorityKey below are the only place that
    // handle is translated to/from the canonical preimage. Visibility is
    // `internal` for the same reason as the other key builders above --
    // StoreRecordGoldenTests drives this production preimage directly.
    internal static ZLinkStoreKey AuthorityMetaKey(ZLinkAuthorityKey key)
    {
        if (ZLinkAuthorityKeyCodec.TryDecodeActor(key, out var actorId))
            return OpaqueRecordKey("authority", "actor", actorId);
        if (ZLinkAuthorityKeyCodec.TryDecodeSpot(key, out var spotId))
            return OpaqueRecordKey("authority", "spot", spotId);
        throw new InvalidDataException(
            $"Authority key '{key.Value}' is not canonical authority-key-v1."
        );
    }

    private static ZLinkStoreKey AuthorityOwnerGenerationCounterKey(ZLinkAuthorityKey key) =>
        GenerationCounterKey("authority-owner-counter", key);

    private static ZLinkStoreKey ObjectGenerationCounterKey(ZLinkAuthorityKey key) =>
        GenerationCounterKey("object-counter", key);

    private static ZLinkStoreKey GenerationCounterKey(string counter, ZLinkAuthorityKey key) =>
        Key($"zlink:v12:{counter}{AuthorityMetaKey(key).Value["authority".Length..]}");

    private const string AuthorityCanonicalPreimagePrefix = "authority\0";

    // ListAuthoritiesAsync's `prefix` parameter (IZLinkLocationRepository)
    // is the pre-existing "zla1:a:"/"zla1:s:" kind marker every caller
    // passes (ZLinkLocationObjectQuery, ZLinkInstanceSpotActivationTarget) -- translated here to the
    // canonical preimage's kind segment for the real opaque-record scan.
    // An empty prefix (scan everything) passes through unchanged.
    private static string AuthorityMetaPrefix(string prefix) =>
        prefix switch
        {
            "" => AuthorityCanonicalPreimagePrefix,
            "zla1:a:" => $"{AuthorityCanonicalPreimagePrefix}actor\0",
            "zla1:s:" => $"{AuthorityCanonicalPreimagePrefix}spot\0",
            _ => throw new ArgumentException(
                $"unsupported authority scan prefix '{prefix}'",
                nameof(prefix)
            ),
        };

    private static ZLinkAuthorityKey DecodeAuthorityKey(ZLinkStoreKey key)
    {
        var preimage = key.Value;
        if (!preimage.StartsWith(AuthorityCanonicalPreimagePrefix, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Authority scan returned a non-authority logical key " + $"'{preimage}'."
            );
        }

        var rest = preimage[AuthorityCanonicalPreimagePrefix.Length..];
        var separator = rest.IndexOf('\0');
        if (separator < 0)
        {
            throw new InvalidDataException("Authority logical key preimage is malformed.");
        }

        var kind = rest[..separator];
        var id = rest[(separator + 1)..];
        return kind switch
        {
            "actor" => ZLinkAuthorityKeyCodec.EncodeActor(id),
            "spot" => ZLinkAuthorityKeyCodec.EncodeSpot(id),
            _ => throw new InvalidDataException($"Unrecognized authority key kind '{kind}'."),
        };
    }

    internal static ZLinkStoreKey TerminalKey(ZLinkCreationOperationId operation) =>
        OpaqueRecordKey(
            "creation-terminal",
            operation.SourceNodeRid.ToHex().ToLowerInvariant(),
            operation.SourceNodeGeneration.ToString(CultureInfo.InvariantCulture),
            CreationOperationIdHex(operation)
        );

    private static string CreationOperationIdHex(ZLinkCreationOperationId operation) =>
        operation.OperationIdHigh.ToString("x16", CultureInfo.InvariantCulture)
        + $"{operation.OperationIdLow.ToString("x16", CultureInfo.InvariantCulture)}";

    private static ZLinkStoreKey AggregateKey(ZLinkAggregateFence fence) =>
        ZLinkAggregateInventory.AggregateKey(Prefix, fence);

    private static ZLinkStoreKey AggregateParticipantMetaKey(
        ZLinkAggregateFence fence,
        int index
    ) => Key(AggregateKey(fence).Value + FormattableString.Invariant($":participant:{index}:meta"));

    // Canonical single-row authority envelope (21-location-runtime.md#2.4):
    // Payload is the application-opaque bytes directly -- there is no
    // separate payload key or checksum to reconcile a split read against
    // (checklist C-2b).
    // Canonical field names/types per 21-location-runtime.md#2.4's
    // authority table: generation and lease fields are decimal-string
    // JSON (64-bit values can exceed JSON number precision), payload
    // is base64 (System.Text.Json's byte[] default), and the pending
    // -creation field is named pendingCreation on the wire (kept as
    // ReservedCreation in C# so existing `with` expressions and property
    // access across this file don't need to change).
    private sealed record AuthorityMeta(
        [property: JsonPropertyName("payload")] byte[] Payload,
        [property: JsonPropertyName("objectGeneration")]
        [property: JsonConverter(
            typeof(Messaging.ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter)
        )]
            ulong ObjectGeneration,
        [property: JsonPropertyName("authorityOwnerGeneration")]
        [property: JsonConverter(
            typeof(Messaging.ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter)
        )]
            ulong AuthorityOwnerGeneration,
        [property: JsonPropertyName("ownerId")] string OwnerId,
        [property: JsonPropertyName("ownerLeaseGeneration")]
        [property: JsonConverter(
            typeof(Messaging.ZLinkJsonSerializerOptions.FrameworkSigned64JsonConverter)
        )]
            long OwnerLeaseGeneration,
        [property: JsonPropertyName("allocation")] ZLinkPlacementAllocation Allocation,
        [property: JsonPropertyName("pendingCreation")]
            ZLinkReservedObjectCreation? ReservedCreation
    )
    {
        [JsonPropertyName("recordVersion")]
        [JsonPropertyOrder(-1)]
        public int RecordVersion { get; init; } = 1;

        [JsonPropertyName("aggregate")]
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public JsonElement? Aggregate { get; init; }

        [JsonPropertyName("visibleStoreVersion")]
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public string? VisibleStoreVersion { get; init; }

        [JsonPropertyName("aggregateFence")]
        public ZLinkAggregateFence? AggregateFence { get; init; }

        [JsonPropertyName("aggregateParticipantIndex")]
        public int? AggregateParticipantIndex { get; init; }

        [JsonPropertyName("aggregateExpectedStoreVersion")]
        public string? AggregateExpectedStoreVersion { get; init; }
    }

    private sealed record StoredAuthority(
        ZLinkAuthorityKey Key,
        AuthorityMeta Meta,
        ZLinkStoreVersion Version,
        ZLinkAuthoritySnapshot Snapshot
    );

    private sealed record StoredTerminal(
        ZLinkCreationTerminalRecord Record,
        ZLinkStoreVersion Version
    );

    private sealed record AggregateHeader(
        Guid AggregateId,
        ulong AggregateGeneration,
        byte[] InventoryDigest,
        ZLinkMeshNodeDescriptorKey TargetDescriptor,
        ulong TargetDescriptorLifecycleGeneration,
        ZLinkCapacityVector Capacity,
        ZLinkLocationOwnerToken TargetOwner
    )
    {
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public ZLinkAuthorityRelocation? Relocation { get; init; }

        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
        public bool ClearRelocation { get; init; }

        public ZLinkAuthorityKey? RepresentativeKey { get; init; }

        internal ReadOnlyMemory<byte> ProjectPayload(
            ZLinkPlacementObjectKind kind,
            ReadOnlyMemory<byte> payload
        ) =>
            RepresentativeKey is null
                ? payload
                : ZLinkAggregateRelocationCoordinator.ProjectParticipantPayload(
                    kind,
                    payload,
                    TargetDescriptor,
                    TargetDescriptorLifecycleGeneration,
                    TargetOwner,
                    AggregateId
                );

        internal JsonElement? ApplyRelocation(JsonElement? aggregate) =>
            ClearRelocation || Relocation is not null
                ? ZLinkAuthorityAggregateCodec.Replace(aggregate, Relocation)
                : aggregate;
    }

    private sealed record AggregateRecord(
        AggregateHeader Header,
        int ParticipantCount,
        byte[] RequestFingerprint,
        AggregateStatus Status,
        DateTimeOffset StagedAt,
        bool ParticipantsNormalized,
        bool ParticipantsCleaned
    );

    private sealed record AggregateParticipantRecord(
        int Index,
        string Key,
        string ExpectedStoreVersion,
        ZLinkAuthorityGenerationTransition OwnerTransition,
        ulong TargetAuthorityOwnerGeneration,
        byte[] RequestFingerprint
    );

    private sealed record StoredAggregateParticipant(
        AggregateParticipantRecord Metadata,
        ReadOnlyMemory<byte> Payload,
        ReadOnlyMemory<byte> MembershipMutation
    );

    private sealed record StoredRecord<T>(
        T Record,
        ZLinkStoreVersion Version,
        DateTimeOffset StoreNow
    );

    private sealed record StoredOwner(ZLinkLocationOwnerToken Token, ReadOnlyMemory<byte> Value);

    private sealed record StoredTarget(
        ZLinkMeshNodeDescriptor Descriptor,
        ZLinkStoreCondition DescriptorCondition,
        ZLinkStoreCondition OwnerCondition
    );

    private readonly record struct GenerationCounter(
        ZLinkStoreKey Key,
        ulong Value,
        ZLinkStoreCondition Condition
    )
    {
        internal ulong Issue(
            ICollection<ZLinkStoreCondition> conditions,
            ICollection<ZLinkStoreMutation> mutations
        )
        {
            conditions.Add(Condition);
            mutations.Add(
                new ZLinkStoreMutation.Put(Key, EncodeGenerationCounter(Value + 1), null)
            );
            return Value;
        }
    }

    private enum StaleAuthorityReclaimResult
    {
        OwnerLive = 1,
        Reclaimed = 2,
        Conflict = 3,
        RecoveryRequired = 4,
        NotReclaimable = 5,
    }

    private enum AggregateStatus
    {
        Staging = 0,
        Prepared = 1,
        Committed = 2,
        Aborted = 3,
    }
}
