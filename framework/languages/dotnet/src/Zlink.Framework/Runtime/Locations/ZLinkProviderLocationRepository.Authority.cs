using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Buffers.Binary;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private const string AuthorityPrefix = Prefix + "authority:";
    private const string ReservationPrefix = Prefix + "creation-reservation:";
    private const string TerminalPrefix = Prefix + "creation-terminal:";
    private const string RelocationCapacityPrefix = Prefix + "relocation-capacity:";
    private const string AggregatePrefix = Prefix + "aggregate:";
    private static readonly TimeSpan AmbiguousReconciliationTimeout =
        TimeSpan.FromSeconds(5);
    private static readonly TimeSpan CounterRetryWindow =
        TimeSpan.FromSeconds(5);
    private static readonly TimeSpan AggregateStagingRetention =
        TimeSpan.FromHours(24);
    private const int CounterRetryLimit = 64;
    private const int MaxInventoryPageEntries = 1024;
    private const int MaxInventoryPageBytes = 1024 * 1024;
    private const int MaxInventoryTreeLevels = 32;
    private const uint InventoryCodecMagic = 0x5A4C4956; // ZLIV
    private const byte InventoryCodecVersion = 1;
    private const byte InventoryRootRecordKind = 1;
    private const byte InventoryPageRecordKind = 2;
    private const byte InventoryLeafEntryRecordKind = 3;
    private const uint AggregateFingerprintMagic = 0x5A4C4146; // ZLAF
    private const byte AggregateFingerprintVersion = 1;
    private const byte AggregateHeaderRecordKind = 1;
    private const byte AggregateRequestRecordKind = 2;
    private static readonly UTF8Encoding InventoryUtf8 =
        new(false, true);
    private readonly SemaphoreSlim authorityGenerationGate = new(1, 1);
    private readonly SemaphoreSlim aggregateRecoveryGate = new(1, 1);
    private int aggregateStagingRecoveryCompleted;

    public async ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken = default)
    {
        ValidateAuthorityKey(key);
        var stored = await ReadAuthorityRecordAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return stored is null
            ? new ZLinkAuthorityReadResult.Missing(
                await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false))
            : new ZLinkAuthorityReadResult.Found(stored.Snapshot);
    }

    public async ValueTask<ZLinkAuthorityCompareExchangeResult>
        CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default)
    {
        // Only the complete NewOwner handoff has an auxiliary capacity fence.
        // Preserve and Delete conflicts must retain their caller-visible
        // single-CAS semantics; retrying those here could hide an owner or
        // lifecycle transition from their coordinator.
        var retriesCapacityContention = mutation is ZLinkAuthorityMutation.Put
        {
            GenerationTransition: ZLinkAuthorityGenerationTransition.NewOwner,
            RelocationCapacityFence: not null
        };
        if (!retriesCapacityContention)
            return await CompareExchangeAuthorityCoreAsync(
                    key,
                    expectedStoreVersion,
                    mutation,
                    cancellationToken)
                .ConfigureAwait(false);

        // A NewOwner CAS also fences source and target capacity records. Those
        // records can change while the authority version remains unchanged.
        // Rebuild the complete atomic write for that narrow case; returning the
        // first provider conflict would turn unrelated capacity contention into
        // a source-side handoff failure.
        const int maximumCapacityContentionRetries = 8;
        for (var attempt = 0; ; attempt++)
        {
            var result = await CompareExchangeAuthorityCoreAsync(
                    key,
                    expectedStoreVersion,
                    mutation,
                    cancellationToken)
                .ConfigureAwait(false);
            if (attempt >= maximumCapacityContentionRetries
                || result is not ZLinkAuthorityCompareExchangeResult.Conflict
                    {
                        Current: ZLinkAuthorityReadResult.Found current
                    }
                || !string.Equals(
                    current.Snapshot.StoreVersion,
                    expectedStoreVersion,
                    StringComparison.Ordinal)
                || current.Snapshot.Allocation.State
                   != ZLinkPlacementAllocationState.Active)
                return result;

            var delayMilliseconds = Math.Min(32, 1 << Math.Min(attempt, 5));
            await Task.Delay(
                    TimeSpan.FromMilliseconds(delayMilliseconds),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask<ZLinkAuthorityCompareExchangeResult>
        CompareExchangeAuthorityCoreAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken)
    {
        ValidateAuthorityKey(key);
        ArgumentException.ThrowIfNullOrWhiteSpace(expectedStoreVersion);
        ArgumentNullException.ThrowIfNull(mutation);
        ValidateAuthorityMutation(mutation);

        var metaKey = AuthorityMetaKey(key);
        var payloadKey = AuthorityPayloadKey(key);
        var current = await ReadAuthorityRecordAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (current is null
            || current.Snapshot.Allocation.State
            != ZLinkPlacementAllocationState.Active
            || current.Version.Value != expectedStoreVersion
            || current.Meta.AggregateFence is not null)
        {
            if (mutation is ZLinkAuthorityMutation.Delete
                && current is not null
                && current.Version.Value != expectedStoreVersion)
            {
                // A source handoff cleanup can arrive after the target has
                // published a newer authority version. Submit the old
                // metadata fence to the opaque provider so an in-flight
                // cleanup remains observable and retryable. The expected
                // version cannot match the target row, so this batch cannot
                // delete the target metadata or payload.
                await provider.WriteAsync(
                        new ZLinkStoreWriteRequest(
                            [new ZLinkStoreCondition.Version(
                                metaKey,
                                new ZLinkStoreVersion(expectedStoreVersion))],
                            [
                                new ZLinkStoreMutation.Delete(metaKey),
                                new ZLinkStoreMutation.Delete(payloadKey)
                            ]),
                        cancellationToken)
                    .ConfigureAwait(false);
            }

            //  Four separate reasons collapse into one Conflict, and the caller
            //  reports all of them as "authority changed".
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"cas_conflict_reason missing={current is null} "
                + $"state={(current is null ? "n/a" : current.Snapshot.Allocation.State.ToString())} "
                + $"version_match={current is not null && current.Version.Value == expectedStoreVersion} "
                + $"fence={(current is null ? "n/a" : current.Meta.AggregateFence?.ToString() ?? "none")}");
            return Conflict(current);
        }

        if (mutation is ZLinkAuthorityMutation.Restore restore)
        {
            ValidateAuthorityPayload(restore.Payload);
            if (current.Snapshot.OwnerId != restore.ExpectedOwner.OwnerId
                || current.Snapshot.OwnerLeaseGeneration
                != restore.ExpectedOwner.LeaseGeneration)
                return Conflict(current);
            return await StoreAuthorityAsync(
                    current,
                    current.Meta with { PayloadSha256 = Sha256(restore.Payload) },
                    restore.Payload,
                    [new ZLinkStoreCondition.Version(metaKey, current.Version)],
                    [],
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (mutation is ZLinkAuthorityMutation.Delete)
        {
            var owner = await ReadLiveOwnerAsync(
                    new ZLinkLocationOwnerToken(
                        current.Snapshot.OwnerId,
                        current.Snapshot.OwnerLeaseGeneration),
                    cancellationToken)
                .ConfigureAwait(false);
            if (owner is null) return Conflict(current);
            var target = await ReadCapacityAsync(
                    current.Snapshot.Allocation.Descriptor,
                    current.Snapshot.Allocation.DescriptorLifecycleGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
            var capacity = target.Record.Clone();
            ApplyCapacity(capacity, current.Snapshot.Allocation, activeDelta: -1);
            var result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(
                    [
                        new ZLinkStoreCondition.Version(metaKey, current.Version),
                        new ZLinkStoreCondition.Version(
                            OwnerKey(owner.Token.OwnerId),
                            owner.Version),
                        target.Condition
                    ],
                    [
                        new ZLinkStoreMutation.Delete(metaKey),
                        new ZLinkStoreMutation.Delete(payloadKey),
                        new ZLinkStoreMutation.Put(
                            target.Key,
                            Encode(capacity),
                            null)
                    ]),
                    cancellationToken)
                .ConfigureAwait(false);
            return result is ZLinkStoreWriteResult.Applied applied
                ? new ZLinkAuthorityCompareExchangeResult.Deleted(
                    applied.PutVersions[target.Key].Value,
                    applied.StoreNow)
                : Conflict(await ReadAuthorityRecordAsync(key, cancellationToken)
                    .ConfigureAwait(false));
        }

        var put = (ZLinkAuthorityMutation.Put)mutation;
        ValidateAuthorityPayload(put.Payload);
        var changesOwner = put.GenerationTransition
                           == ZLinkAuthorityGenerationTransition.NewOwner;
        var targetOwner = put.TargetOwner
                          ?? new ZLinkLocationOwnerToken(
                              current.Snapshot.OwnerId,
                              current.Snapshot.OwnerLeaseGeneration);
        var liveOwner = await ReadLiveOwnerAsync(targetOwner, cancellationToken)
            .ConfigureAwait(false);
        if (liveOwner is null) return Conflict(current);

        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(metaKey, current.Version),
            new ZLinkStoreCondition.Version(
                OwnerKey(targetOwner.OwnerId),
                liveOwner.Version)
        };
        var mutations = new List<ZLinkStoreMutation>();
        var nextAllocation = current.Snapshot.Allocation;
        RelocationRecordState? relocation = null;
        StoredRecord<RelocationRecordState>? storedRelocation = null;
        StoredCapacity? sourceCapacity = null;
        StoredCapacity? targetCapacity = null;
        if (put.RelocationCapacityFence is { } fence)
        {
            storedRelocation = await ReadRecordAsync<RelocationRecordState>(
                    RelocationKey(fence),
                    cancellationToken)
                .ConfigureAwait(false);
            relocation = storedRelocation?.Record;
            if (relocation is null
                || relocation.Status is not (RelocationStatus.Reserved
                    or RelocationStatus.Prepared)
                || relocation.Request.Key != key
                || relocation.Request.SourceOwner
                != new ZLinkLocationOwnerToken(
                    current.Snapshot.OwnerId,
                    current.Snapshot.OwnerLeaseGeneration)
                || !MatchesSourceAllocation(
                    current.Snapshot.Allocation,
                    relocation.Request))
                return Conflict(current);
            conditions.Add(new ZLinkStoreCondition.Version(
                RelocationKey(fence),
                storedRelocation!.Version));

            if (changesOwner)
            {
                if (relocation.Request.TargetOwner != targetOwner
                    || !await IsEligibleTargetAsync(
                            relocation.Request.TargetDescriptor,
                            relocation.Request.TargetNodeLifecycleGeneration,
                            targetOwner,
                            relocation.Request.ObjectKind,
                            relocation.Request.StableType,
                            cancellationToken)
                        .ConfigureAwait(false))
                    return Conflict(current);
                sourceCapacity = await ReadCapacityAsync(
                        current.Snapshot.Allocation.Descriptor,
                        current.Snapshot.Allocation.DescriptorLifecycleGeneration,
                        cancellationToken)
                    .ConfigureAwait(false);
                targetCapacity = await ReadCapacityAsync(
                        relocation.Request.TargetDescriptor,
                        relocation.Request.TargetNodeLifecycleGeneration,
                        cancellationToken)
                    .ConfigureAwait(false);
                AddCondition(conditions, sourceCapacity.Condition);
                AddCondition(conditions, targetCapacity.Condition);
                var source = sourceCapacity.Record.Clone();
                var target = sourceCapacity.Key == targetCapacity.Key
                    ? source
                    : targetCapacity.Record.Clone();
                ApplyCapacity(source, current.Snapshot.Allocation, activeDelta: -1);
                ApplyCapacity(
                    target,
                    TargetAllocation(relocation.Request),
                    pendingDelta: -1,
                    activeDelta: 1);
                mutations.Add(new ZLinkStoreMutation.Put(
                    sourceCapacity.Key,
                    Encode(source),
                    null));
                if (targetCapacity.Key != sourceCapacity.Key)
                    mutations.Add(new ZLinkStoreMutation.Put(
                        targetCapacity.Key,
                        Encode(target),
                        null));
                relocation = relocation with { Status = RelocationStatus.Committed };
                nextAllocation = new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Active,
                    relocation.Request.ObjectKind,
                    relocation.Request.StableType,
                    relocation.Request.TargetDescriptor,
                    relocation.Request.TargetNodeLifecycleGeneration,
                    relocation.Request.Capacity);
            }
            else
            {
                relocation = relocation with { Status = RelocationStatus.Prepared };
            }
            mutations.Add(new ZLinkStoreMutation.Put(
                RelocationKey(fence),
                Encode(relocation),
                null));
        }
        else if (changesOwner)
        {
            return Conflict(current);
        }

        var nextAuthorityOwnerGeneration =
            current.Meta.AuthorityOwnerGeneration;
        if (changesOwner)
        {
            var generation = await ReadGenerationAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (generation.ObjectGeneration != current.Meta.ObjectGeneration
                || generation.AuthorityOwnerGeneration
                != current.Meta.AuthorityOwnerGeneration)
                return Conflict(current);
            nextAuthorityOwnerGeneration =
                relocation!.TargetAuthorityOwnerGeneration;
            if (nextAuthorityOwnerGeneration
                    <= generation.AuthorityOwnerGeneration
                || nextAuthorityOwnerGeneration > long.MaxValue)
                return Conflict(current);
            conditions.Add(generation.Condition);
            mutations.Add(new ZLinkStoreMutation.Put(
                GenerationKey(key),
                Encode(new GenerationRecord(
                    generation.ObjectGeneration,
                    nextAuthorityOwnerGeneration)),
                null));
        }
        var meta = current.Meta with
        {
            PayloadSha256 = Sha256(put.Payload),
            AuthorityOwnerGeneration = nextAuthorityOwnerGeneration,
            OwnerId = targetOwner.OwnerId,
            OwnerLeaseGeneration = targetOwner.LeaseGeneration,
            Allocation = nextAllocation
        };
        return await StoreAuthorityAsync(
                current,
                meta,
                put.Payload,
                conditions,
                mutations,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
        string prefix,
        ZLinkAuthorityScanCursor? cursor,
        int limit,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(prefix);
        if (limit is < 1 or > 1000)
            throw new ArgumentOutOfRangeException(nameof(limit));
        var result = await provider.ScanAsync(
                new ZLinkStoreScanRequest(
                    AuthorityMetaPrefix(prefix),
                    cursor is null
                        ? null
                        : new ZLinkStoreScanCursor(cursor.Value.Encoded),
                    limit),
                cancellationToken)
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
                    cancellationToken)
                .ConfigureAwait(false);
            if (stored is not null)
                entries.Add(new ZLinkAuthorityEntry(key, stored.Snapshot));
        }
        return new ZLinkAuthorityScanResult.Page(
            new ZLinkAuthorityPage(
                entries,
                page.NextCursor is null
                    ? null
                    : new ZLinkAuthorityScanCursor(page.NextCursor.Value.Value)));
    }

    public async ValueTask<ZLinkObjectReserveResult> ReserveAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken = default)
    {
        ValidateReservation(request);
        await authorityGenerationGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            return await ReserveCoreAsync(
                    request,
                    0,
                    DateTimeOffset.UtcNow + CounterRetryWindow,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            authorityGenerationGate.Release();
        }
    }

    private async ValueTask<ZLinkObjectReserveResult> ReserveCoreAsync(
        ZLinkObjectReservationRequest request,
        int counterRetry,
        DateTimeOffset retryDeadline,
        CancellationToken cancellationToken)
    {
        ValidateReservation(request);
        var current = await ReadAuthorityRecordAsync(request.Key, cancellationToken)
            .ConfigureAwait(false);
        if (current is not null)
        {
            var reclaim = await TryReclaimStaleAuthorityAsync(
                    current,
                    cancellationToken)
                .ConfigureAwait(false);
            if (reclaim == StaleAuthorityReclaimResult.Reclaimed)
                return await ReserveCoreAsync(
                        request,
                        counterRetry,
                        retryDeadline,
                        cancellationToken)
                    .ConfigureAwait(false);
            if (reclaim == StaleAuthorityReclaimResult.Conflict
                && counterRetry < CounterRetryLimit
                && DateTimeOffset.UtcNow < retryDeadline)
            {
                await DelayCounterRetryAsync(
                        counterRetry,
                        retryDeadline,
                        cancellationToken)
                    .ConfigureAwait(false);
                return await ReserveCoreAsync(
                        request,
                        counterRetry + 1,
                        retryDeadline,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            return current.Snapshot.Allocation.State
                   == ZLinkPlacementAllocationState.Active
                ? new ZLinkObjectReserveResult.AlreadyExists(current.Snapshot)
                : new ZLinkObjectReserveResult.Conflict(
                    new ZLinkAuthorityReadResult.Found(current.Snapshot));
        }
        var target = await ReadEligibleTargetAsync(
                request.TargetDescriptor,
                request.TargetNodeLifecycleGeneration,
                request.TargetOwner,
                request.ObjectKind,
                request.StableType,
                cancellationToken)
            .ConfigureAwait(false);
        if (target is null)
            return new ZLinkObjectReserveResult.Conflict(
                new ZLinkAuthorityReadResult.Missing(
                    await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false)));

        var capacity = await ReadCapacityAsync(
                request.TargetDescriptor,
                request.TargetNodeLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        if (!HasCapacity(target.Descriptor, capacity.Record, request.Capacity))
            return new ZLinkObjectReserveResult.PlacementCapacityExhausted();
        var generation = await ReadGenerationAsync(request.Key, cancellationToken)
            .ConfigureAwait(false);
        var authorityCounter =
            await ReadAuthorityOwnerGenerationCounterAsync(cancellationToken)
                .ConfigureAwait(false);
        var authorityHighWater = Math.Max(
            authorityCounter.Value,
            generation.AuthorityOwnerGeneration);
        if (generation.ObjectGeneration == ulong.MaxValue
            || authorityHighWater == long.MaxValue)
            return new ZLinkObjectReserveResult.GenerationExhausted();

        var objectGeneration = generation.ObjectGeneration + 1;
        var authorityOwnerGeneration = authorityHighWater + 1;
        var reservationId = Guid.NewGuid().ToString("N");
        var allocation = new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.Reserved,
            request.ObjectKind,
            request.StableType,
            request.TargetDescriptor,
            request.TargetNodeLifecycleGeneration,
            request.Capacity);
        var meta = new AuthorityMeta(
            Sha256(request.CreatingPayload),
            objectGeneration,
            authorityOwnerGeneration,
            request.TargetOwner.OwnerId,
            request.TargetOwner.LeaseGeneration,
            allocation,
            new ZLinkReservedObjectCreation(
                reservationId,
                request.CreationIntentReference,
                request.CreationIntentHash.ToArray(),
                request.CreationIntentEncodedSize));
        var reservation = new ReservationRecord(
            request.Key,
            objectGeneration,
            authorityOwnerGeneration,
            reservationId,
            request.TargetDescriptor,
            request.TargetNodeLifecycleGeneration,
            request.TargetOwner,
            ReservationStatus.Reserved);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacity(nextCapacity, allocation, pendingDelta: 1);
        var metaKey = AuthorityMetaKey(request.Key);
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Missing(metaKey),
                    generation.Condition,
                    target.DescriptorCondition,
                    target.OwnerCondition,
                    capacity.Condition,
                    authorityCounter.Condition,
                    new ZLinkStoreCondition.Missing(
                        ReservationKey(reservationId))
                ],
                [
                    new ZLinkStoreMutation.Put(metaKey, Encode(meta), null),
                    new ZLinkStoreMutation.Put(
                        AuthorityPayloadKey(request.Key),
                        request.CreatingPayload.ToArray(),
                        null),
                    new ZLinkStoreMutation.Put(
                        GenerationKey(request.Key),
                        Encode(new GenerationRecord(
                            objectGeneration,
                            authorityOwnerGeneration)),
                        null),
                    new ZLinkStoreMutation.Put(
                        ReservationKey(reservationId),
                        Encode(reservation),
                        null),
                    new ZLinkStoreMutation.Put(
                        capacity.Key,
                        Encode(nextCapacity),
                        null),
                    new ZLinkStoreMutation.Put(
                        AuthorityOwnerGenerationCounterKey(),
                        Encode(new AuthorityOwnerGenerationCounter(
                            authorityOwnerGeneration)),
                        null)
                ]),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is ZLinkStoreWriteResult.Conflict
            && counterRetry < CounterRetryLimit
            && DateTimeOffset.UtcNow < retryDeadline
            && await ReadAuthorityRecordAsync(
                    request.Key,
                    cancellationToken)
                .ConfigureAwait(false) is null)
        {
            await DelayCounterRetryAsync(
                    counterRetry,
                    retryDeadline,
                    cancellationToken)
                .ConfigureAwait(false);
            return await ReserveCoreAsync(
                    request,
                    counterRetry + 1,
                    retryDeadline,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        if (result is ZLinkStoreWriteResult.Conflict)
            return await ReserveConflictAsync(request.Key, cancellationToken)
                .ConfigureAwait(false);
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
                request.TargetOwner));
    }

    private async ValueTask<StaleAuthorityReclaimResult>
        TryReclaimStaleAuthorityAsync(
            StoredAuthority current,
            CancellationToken cancellationToken)
    {
        var ownerKey = OwnerKey(current.Snapshot.OwnerId);
        var ownerRead = await provider.ReadAsync(ownerKey, cancellationToken)
            .ConfigureAwait(false);
        ZLinkStoreCondition staleOwnerCondition;
        if (ownerRead is ZLinkStoreReadResult.Missing)
        {
            staleOwnerCondition = new ZLinkStoreCondition.Missing(ownerKey);
        }
        else
        {
            var ownerFound = (ZLinkStoreReadResult.Found)ownerRead;
            var owner = Decode<OwnerRecord>(ownerFound.Value.Bytes);
            if (!string.Equals(
                    owner.OwnerId,
                    current.Snapshot.OwnerId,
                    StringComparison.Ordinal))
                throw new InvalidDataException(
                    "The Location Store owner lease record is invalid.");
            if (owner.LeaseGeneration == current.Snapshot.OwnerLeaseGeneration)
                return StaleAuthorityReclaimResult.OwnerLive;
            staleOwnerCondition = new ZLinkStoreCondition.Version(
                ownerKey,
                ownerFound.Value.Version);
        }

        // A relocation record has its own recovery protocol. GetOrCreate may
        // reclaim only a steady authority or an unfinished creation whose
        // owner lease ended.
        if (current.Meta.AggregateFence is not null
            || ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Snapshot.Payload.Span,
                out _))
            return StaleAuthorityReclaimResult.RecoveryRequired;

        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(
                AuthorityMetaKey(current.Key),
                current.Version),
            staleOwnerCondition
        };
        var mutations = new List<ZLinkStoreMutation>
        {
            new ZLinkStoreMutation.Delete(AuthorityMetaKey(current.Key)),
            new ZLinkStoreMutation.Delete(AuthorityPayloadKey(current.Key))
        };
        var capacity = await ReadCapacityAsync(
                current.Snapshot.Allocation.Descriptor,
                current.Snapshot.Allocation.DescriptorLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var nextCapacity = capacity.Record.Clone();
        conditions.Add(capacity.Condition);
        if (current.Snapshot.Allocation.State
            == ZLinkPlacementAllocationState.Active)
        {
            ApplyCapacity(
                nextCapacity,
                current.Snapshot.Allocation,
                activeDelta: -1);
        }
        else
        {
            if (current.Meta.ReservedCreation is not { } reservation)
                return StaleAuthorityReclaimResult.RecoveryRequired;
            var storedReservation = await ReadRecordAsync<ReservationRecord>(
                    ReservationKey(reservation.ReservationId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (storedReservation is null
                || storedReservation.Record.Status != ReservationStatus.Reserved
                || storedReservation.Record.Key != current.Key
                || storedReservation.Record.ObjectGeneration
                != current.Snapshot.ObjectGeneration
                || storedReservation.Record.AuthorityOwnerGeneration
                != current.Snapshot.AuthorityOwnerGeneration)
                return StaleAuthorityReclaimResult.RecoveryRequired;
            conditions.Add(new ZLinkStoreCondition.Version(
                ReservationKey(reservation.ReservationId),
                storedReservation.Version));
            mutations.Add(new ZLinkStoreMutation.Put(
                ReservationKey(reservation.ReservationId),
                Encode(storedReservation.Record with
                {
                    Status = ReservationStatus.Aborted
                }),
                null));
            ApplyCapacity(
                nextCapacity,
                current.Snapshot.Allocation,
                pendingDelta: -1);
        }
        mutations.Add(new ZLinkStoreMutation.Put(
            capacity.Key,
            Encode(nextCapacity),
            null));

        ZLinkStoreWriteResult result;
        try
        {
            result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(conditions, mutations),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            RecordInstanceSpotTakeover(current, "failed");
            throw;
        }
        RecordInstanceSpotTakeover(
            current,
            result is ZLinkStoreWriteResult.Applied ? "claimed" : "lost");
        return result is ZLinkStoreWriteResult.Applied
            ? StaleAuthorityReclaimResult.Reclaimed
            : StaleAuthorityReclaimResult.Conflict;
    }

    private static void RecordInstanceSpotTakeover(
        StoredAuthority current,
        string outcome)
    {
        if (current.Snapshot.Allocation.ObjectKind
            != ZLinkPlacementObjectKind.InstanceSpot)
            return;
        ZLinkRuntimeMetrics.RecordInstanceSpotTakeover(
            current.Snapshot.Allocation.Descriptor.MeshName,
            current.Snapshot.Allocation.StableType,
            outcome);
    }

    public ValueTask<ZLinkObjectCommitResult> CommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        CancellationToken cancellationToken = default) =>
        CompleteCommitAsync(
            reservation,
            readyPayload,
            0,
            DateTimeOffset.UtcNow + CounterRetryWindow,
            cancellationToken);

    public ValueTask<ZLinkObjectCreationCompleteResult>
        CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default) =>
        CompleteCreationCoreAsync(
            reservation,
            completion,
            0,
            DateTimeOffset.UtcNow + CounterRetryWindow,
            cancellationToken);

    private async ValueTask<ZLinkObjectCreationCompleteResult>
        CompleteCreationCoreAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            int counterRetry,
            DateTimeOffset retryDeadline,
            CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(reservation);
        ArgumentNullException.ThrowIfNull(completion);
        var publication = completion switch
        {
            ZLinkObjectCreationCompletion.Created value => value.Terminal,
            ZLinkObjectCreationCompletion.Rejected value => value.Terminal,
            ZLinkObjectCreationCompletion.Failed value => value.Terminal,
            _ => throw new ArgumentOutOfRangeException(nameof(completion))
        };
        ValidateTerminal(publication);
        var terminalKey = TerminalKey(publication.Operation);
        var existingTerminal = await ReadTerminalAsync(
                publication.Operation,
                cancellationToken)
            .ConfigureAwait(false);
        if (existingTerminal is not null)
            return new ZLinkObjectCreationCompleteResult.AlreadyCompleted(
                existingTerminal.Record);

        var current = await ReadAuthorityRecordAsync(
                reservation.Key,
                cancellationToken)
            .ConfigureAwait(false);
        var storedReservation = await ReadRecordAsync<ReservationRecord>(
                ReservationKey(reservation.ReservationVersion),
                cancellationToken)
            .ConfigureAwait(false);
        if (!MatchesReservation(current, storedReservation, reservation))
            return new ZLinkObjectCreationCompleteResult.Stale();
        if (publication.ExpiresAt <= current!.Snapshot.StoreNow)
            throw new ArgumentOutOfRangeException(
                nameof(completion),
                "The creation terminal must expire after StoreNow.");
        var target = await ReadEligibleTargetAsync(
                reservation.TargetDescriptor,
                reservation.TargetNodeLifecycleGeneration,
                reservation.TargetOwner,
                current!.Snapshot.Allocation.ObjectKind,
                current.Snapshot.Allocation.StableType,
                cancellationToken,
                requireNewPlacementEligibility: false)
            .ConfigureAwait(false);
        if (target is null)
            return new ZLinkObjectCreationCompleteResult.Stale();
        var capacity = await ReadCapacityAsync(
                reservation.TargetDescriptor,
                reservation.TargetNodeLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacity(
            nextCapacity,
            current.Snapshot.Allocation,
            pendingDelta: -1,
            activeDelta: completion is ZLinkObjectCreationCompletion.Created ? 1 : 0);
        var state = completion switch
        {
            ZLinkObjectCreationCompletion.Created =>
                ZLinkCreationTerminalState.Created,
            ZLinkObjectCreationCompletion.Rejected =>
                ZLinkCreationTerminalState.Rejected,
            _ => ZLinkCreationTerminalState.Failed
        };
        var terminal = new ZLinkCreationTerminalRecord(
            publication.Operation,
            reservation.ReservationVersion,
            current.Snapshot.Allocation.ObjectKind,
            state,
            publication.TerminalEnvelope.ToArray(),
            publication.TerminalEnvelopeSha256.ToArray(),
            publication.ExpiresAt,
            current.Snapshot.StoreNow);
        var terminalMeta = new TerminalMeta(
            terminal with
            {
                TerminalEnvelope = ReadOnlyMemory<byte>.Empty,
                StoreNow = default
            },
            Sha256(publication.TerminalEnvelope));
        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(
                AuthorityMetaKey(reservation.Key),
                current.Version),
            new ZLinkStoreCondition.Version(
                ReservationKey(reservation.ReservationVersion),
                storedReservation!.Version),
            new ZLinkStoreCondition.Missing(terminalKey),
            target.DescriptorCondition,
            target.OwnerCondition,
            capacity.Condition
        };
        var reservationRecord = storedReservation.Record with
        {
            Status = state == ZLinkCreationTerminalState.Created
                ? ReservationStatus.Created
                : state == ZLinkCreationTerminalState.Rejected
                    ? ReservationStatus.Rejected
                    : ReservationStatus.Failed
        };
        var mutations = new List<ZLinkStoreMutation>
        {
            new ZLinkStoreMutation.Put(
                ReservationKey(reservation.ReservationVersion),
                Encode(reservationRecord),
                null),
            new ZLinkStoreMutation.Put(
                terminalKey,
                Encode(terminalMeta),
                publication.ExpiresAt - current.Snapshot.StoreNow),
            new ZLinkStoreMutation.Put(
                TerminalPayloadKey(publication.Operation),
                publication.TerminalEnvelope.ToArray(),
                publication.ExpiresAt - current.Snapshot.StoreNow),
            new ZLinkStoreMutation.Put(capacity.Key, Encode(nextCapacity), null)
        };
        AuthorityMeta? readyMeta = null;
        if (completion is ZLinkObjectCreationCompletion.Created created)
        {
            ValidateAuthorityPayload(created.ReadyPayload);
            readyMeta = current.Meta with
            {
                PayloadSha256 = Sha256(created.ReadyPayload),
                Allocation = current.Snapshot.Allocation with
                {
                    State = ZLinkPlacementAllocationState.Active
                },
                ReservedCreation = null
            };
            mutations.Add(new ZLinkStoreMutation.Put(
                AuthorityMetaKey(reservation.Key),
                Encode(readyMeta),
                null));
            mutations.Add(new ZLinkStoreMutation.Put(
                AuthorityPayloadKey(reservation.Key),
                created.ReadyPayload.ToArray(),
                null));
        }
        else
        {
            mutations.Add(new ZLinkStoreMutation.Delete(
                AuthorityMetaKey(reservation.Key)));
            mutations.Add(new ZLinkStoreMutation.Delete(
                AuthorityPayloadKey(reservation.Key)));
        }
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(conditions, mutations),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is ZLinkStoreWriteResult.Conflict)
        {
            var raced = await ReadTerminalAsync(
                    publication.Operation,
                    cancellationToken)
                .ConfigureAwait(false);
            if (raced is not null)
                return new ZLinkObjectCreationCompleteResult.AlreadyCompleted(
                    raced.Record);

            if (counterRetry < CounterRetryLimit
                && DateTimeOffset.UtcNow < retryDeadline)
            {
                var unchangedAuthority = await ReadAuthorityRecordAsync(
                        reservation.Key,
                        cancellationToken)
                    .ConfigureAwait(false);
                var unchangedReservation =
                    await ReadRecordAsync<ReservationRecord>(
                            ReservationKey(reservation.ReservationVersion),
                            cancellationToken)
                        .ConfigureAwait(false);
                if (MatchesReservation(
                        unchangedAuthority,
                        unchangedReservation,
                        reservation))
                {
                    await DelayCounterRetryAsync(
                            counterRetry,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                    return await CompleteCreationCoreAsync(
                            reservation,
                            completion,
                            counterRetry + 1,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            return new ZLinkObjectCreationCompleteResult.Stale();
        }
        var applied = (ZLinkStoreWriteResult.Applied)result;
        terminal = terminal with { StoreNow = applied.StoreNow };
        return completion switch
        {
            ZLinkObjectCreationCompletion.Created => new
                ZLinkObjectCreationCompleteResult.Created(
                    Snapshot(
                        readyMeta!,
                        applied.PutVersions[
                            AuthorityMetaKey(reservation.Key)],
                        applied.StoreNow,
                        ((ZLinkObjectCreationCompletion.Created)completion)
                        .ReadyPayload),
                    terminal),
            ZLinkObjectCreationCompletion.Rejected =>
                new ZLinkObjectCreationCompleteResult.Rejected(terminal),
            _ => new ZLinkObjectCreationCompleteResult.Failed(terminal)
        };
    }

    public async ValueTask<ZLinkCreationTerminalReadResult>
        ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default)
    {
        ValidateCreationOperation(operation);
        var terminal = await ReadTerminalAsync(operation, cancellationToken)
            .ConfigureAwait(false);
        return terminal is null
            ? new ZLinkCreationTerminalReadResult.Missing(
                await ReadStoreNowAsync(cancellationToken).ConfigureAwait(false))
            : new ZLinkCreationTerminalReadResult.Found(terminal.Record);
    }

    public async ValueTask<ZLinkObjectAbortResult> AbortAsync(
        ZLinkObjectReservation reservation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(reservation);
        var current = await ReadAuthorityRecordAsync(
                reservation.Key,
                cancellationToken)
            .ConfigureAwait(false);
        var storedReservation = await ReadRecordAsync<ReservationRecord>(
                ReservationKey(reservation.ReservationVersion),
                cancellationToken)
            .ConfigureAwait(false);
        if (storedReservation?.Record.Status == ReservationStatus.Aborted)
            return new ZLinkObjectAbortResult.AlreadyAborted();
        if (!MatchesReservation(current, storedReservation, reservation))
            return new ZLinkObjectAbortResult.Stale();
        var capacity = await ReadCapacityAsync(
                reservation.TargetDescriptor,
                reservation.TargetNodeLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacity(nextCapacity, current!.Snapshot.Allocation, pendingDelta: -1);
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        AuthorityMetaKey(reservation.Key),
                        current.Version),
                    new ZLinkStoreCondition.Version(
                        ReservationKey(reservation.ReservationVersion),
                        storedReservation!.Version),
                    capacity.Condition
                ],
                [
                    new ZLinkStoreMutation.Delete(
                        AuthorityMetaKey(reservation.Key)),
                    new ZLinkStoreMutation.Delete(
                        AuthorityPayloadKey(reservation.Key)),
                    new ZLinkStoreMutation.Put(
                        ReservationKey(reservation.ReservationVersion),
                        Encode(storedReservation.Record with
                        {
                            Status = ReservationStatus.Aborted
                        }),
                        null),
                    new ZLinkStoreMutation.Put(
                        capacity.Key,
                        Encode(nextCapacity),
                        null)
                ]),
                cancellationToken)
            .ConfigureAwait(false);
        return result is ZLinkStoreWriteResult.Applied
            ? new ZLinkObjectAbortResult.Aborted()
            : new ZLinkObjectAbortResult.Stale();
    }

    public ValueTask<ZLinkRelocationCapacityReserveResult>
        ReserveRelocationCapacityAsync(
            ZLinkRelocationCapacityReservationRequest request,
            CancellationToken cancellationToken = default) =>
        ReserveRelocationCapacityCoreAsync(
            request,
            0,
            DateTimeOffset.UtcNow + CounterRetryWindow,
            cancellationToken);

    private async ValueTask<ZLinkRelocationCapacityReserveResult>
        ReserveRelocationCapacityCoreAsync(
            ZLinkRelocationCapacityReservationRequest request,
            int counterRetry,
            DateTimeOffset retryDeadline,
            CancellationToken cancellationToken)
    {
        ValidateRelocationRequest(request);
        var fence = new ZLinkRelocationCapacityFence(
            request.ReservationId.ToString("N"));
        var existing = await ReadRecordAsync<RelocationRecordState>(
                RelocationKey(fence),
                cancellationToken)
            .ConfigureAwait(false);
        if (existing is not null)
            return existing.Record.Request == request
                   && existing.Record.Status == RelocationStatus.Reserved
                   && existing.Record.TargetAuthorityOwnerGeneration > 0
                ? new ZLinkRelocationCapacityReserveResult.AlreadyReserved(fence)
                {
                    TargetAuthorityOwnerGeneration =
                        existing.Record.TargetAuthorityOwnerGeneration
                }
                : new ZLinkRelocationCapacityReserveResult.Conflict(
                    await ReadAuthorityAsync(request.Key, cancellationToken)
                        .ConfigureAwait(false));
        var current = await ReadAuthorityRecordAsync(
                request.Key,
                cancellationToken)
            .ConfigureAwait(false);
        if (current is null
            || current.Version.Value != request.ExpectedStoreVersion
            || current.Snapshot.OwnerId != request.SourceOwner.OwnerId
            || current.Snapshot.OwnerLeaseGeneration
            != request.SourceOwner.LeaseGeneration
            || !MatchesSourceAllocation(current.Snapshot.Allocation, request)
            || current.Meta.AggregateFence is not null)
            return new ZLinkRelocationCapacityReserveResult.Conflict(
                current is null
                    ? new ZLinkAuthorityReadResult.Missing(
                        await ReadStoreNowAsync(cancellationToken)
                            .ConfigureAwait(false))
                    : new ZLinkAuthorityReadResult.Found(current.Snapshot));
        //  The caller named this target, so no selection happens here and the
        //  new-placement rules that govern selection do not apply. Placement
        //  weight steers which node the framework *chooses*; a relocation to an
        //  explicitly addressed target must not be refused because that node is
        //  currently weighted out of the candidate pool. Capacity and liveness
        //  still gate it, through the checks below.
        var targetRead = ReadEligibleTargetAsync(
            request.TargetDescriptor,
            request.TargetNodeLifecycleGeneration,
            request.TargetOwner,
            request.ObjectKind,
            request.StableType,
            cancellationToken,
            requireNewPlacementEligibility: false).AsTask();
        var capacityRead = ReadCapacityAsync(
            request.TargetDescriptor,
            request.TargetNodeLifecycleGeneration,
            cancellationToken).AsTask();
        var generationRead = ReadGenerationAsync(
            request.Key,
            cancellationToken).AsTask();
        var counterRead =
            ReadAuthorityOwnerGenerationCounterAsync(cancellationToken)
                .AsTask();
        await Task.WhenAll(
                targetRead,
                capacityRead,
                generationRead,
                counterRead)
            .ConfigureAwait(false);
        var target = await targetRead.ConfigureAwait(false);
        if (target is null)
            return new ZLinkRelocationCapacityReserveResult.TargetUnavailable();
        var capacity = await capacityRead.ConfigureAwait(false);
        if (!HasCapacity(target.Descriptor, capacity.Record, request.Capacity))
            return new ZLinkRelocationCapacityReserveResult
                .PlacementCapacityExhausted();
        var generation = await generationRead.ConfigureAwait(false);
        if (generation.ObjectGeneration != current.Meta.ObjectGeneration
            || generation.AuthorityOwnerGeneration
               != current.Meta.AuthorityOwnerGeneration)
            return new ZLinkRelocationCapacityReserveResult.Conflict(
                new ZLinkAuthorityReadResult.Found(current.Snapshot));
        var authorityCounter = await counterRead.ConfigureAwait(false);
        var authorityHighWater = Math.Max(
            authorityCounter.Value,
            generation.AuthorityOwnerGeneration);
        if (authorityHighWater == long.MaxValue)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "reserving relocation capacity");
        var targetAuthorityOwnerGeneration = checked(
            authorityHighWater + 1);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacity(
            nextCapacity,
            TargetAllocation(request),
            pendingDelta: 1);
        var conditions = new List<ZLinkStoreCondition>
        {
                    new ZLinkStoreCondition.Version(
                        AuthorityMetaKey(request.Key),
                        current.Version),
                    new ZLinkStoreCondition.Missing(RelocationKey(fence)),
                    target.DescriptorCondition,
                    target.OwnerCondition,
                    capacity.Condition,
                    authorityCounter.Condition
        };
        var mutations = new List<ZLinkStoreMutation>
        {
                    new ZLinkStoreMutation.Put(
                        RelocationKey(fence),
                        Encode(new RelocationRecordState(
                            request,
                            RelocationStatus.Reserved,
                            targetAuthorityOwnerGeneration)),
                        null),
                    new ZLinkStoreMutation.Put(
                        capacity.Key,
                        Encode(nextCapacity),
                        null),
                    new ZLinkStoreMutation.Put(
                        AuthorityOwnerGenerationCounterKey(),
                        Encode(new AuthorityOwnerGenerationCounter(
                            targetAuthorityOwnerGeneration)),
                        null)
        };
        ZLinkStoreWriteResult result;
        try
        {
            result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(conditions, mutations),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            var reconciled =
                await ReadRecordForReconciliationAsync<
                        RelocationRecordState>(
                        RelocationKey(fence))
                .ConfigureAwait(false);
            if (reconciled is not null
                && reconciled.Record.Request == request
                && reconciled.Record.Status == RelocationStatus.Reserved
                && reconciled.Record.TargetAuthorityOwnerGeneration > 0)
                return new ZLinkRelocationCapacityReserveResult
                    .AlreadyReserved(fence)
                {
                    TargetAuthorityOwnerGeneration = reconciled.Record
                            .TargetAuthorityOwnerGeneration
                };
            if (reconciled is not null)
                return new ZLinkRelocationCapacityReserveResult.Conflict(
                    await ReadAuthorityForReconciliationAsync(
                            request.Key)
                        .ConfigureAwait(false));
            throw;
        }
        if (result is ZLinkStoreWriteResult.Conflict
            && counterRetry < CounterRetryLimit
            && DateTimeOffset.UtcNow < retryDeadline)
        {
            var terminal = await ReadRecordAsync<RelocationRecordState>(
                    RelocationKey(fence),
                    cancellationToken)
                .ConfigureAwait(false);
            if (terminal is null)
            {
                var unchanged = await ReadAuthorityRecordAsync(
                        request.Key,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (unchanged is not null
                    && unchanged.Version.Value
                    == request.ExpectedStoreVersion)
                {
                    await DelayCounterRetryAsync(
                            counterRetry,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                    return await ReserveRelocationCapacityCoreAsync(
                            request,
                            counterRetry + 1,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
        }
        return result is ZLinkStoreWriteResult.Applied
            ? new ZLinkRelocationCapacityReserveResult.Reserved(fence)
            {
                TargetAuthorityOwnerGeneration =
                    targetAuthorityOwnerGeneration
            }
            : await ReconcileRelocationCapacityReservationAsync(
                    request,
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkRelocationCapacityAbortResult>
        AbortRelocationCapacityAsync(
            ZLinkRelocationCapacityFence fence,
            CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(fence.Value);
        var stored = await ReadRecordAsync<RelocationRecordState>(
                RelocationKey(fence),
                cancellationToken)
            .ConfigureAwait(false);
        if (stored is null) return ZLinkRelocationCapacityAbortResult.Stale;
        if (stored.Record.Status == RelocationStatus.Aborted)
            return ZLinkRelocationCapacityAbortResult.AlreadyAborted;
        if (stored.Record.Status == RelocationStatus.Committed)
            return ZLinkRelocationCapacityAbortResult.AlreadyCommitted;
        if (stored.Record.Status == RelocationStatus.Prepared)
            return ZLinkRelocationCapacityAbortResult.Stale;
        var request = stored.Record.Request;
        var capacity = await ReadCapacityAsync(
                request.TargetDescriptor,
                request.TargetNodeLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacity(
            nextCapacity,
            TargetAllocation(request),
            pendingDelta: -1);
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        RelocationKey(fence),
                        stored.Version),
                    capacity.Condition
                ],
                [
                    new ZLinkStoreMutation.Put(
                        RelocationKey(fence),
                        Encode(stored.Record with
                        {
                            Status = RelocationStatus.Aborted
                        }),
                        null),
                    new ZLinkStoreMutation.Put(
                        capacity.Key,
                        Encode(nextCapacity),
                        null)
                ]),
                cancellationToken)
            .ConfigureAwait(false);
        return result is ZLinkStoreWriteResult.Applied
            ? ZLinkRelocationCapacityAbortResult.Aborted
            : ZLinkRelocationCapacityAbortResult.Stale;
    }

    private async ValueTask<ZLinkRelocationCapacityReserveResult>
        ReconcileRelocationCapacityReservationAsync(
            ZLinkRelocationCapacityReservationRequest request,
            ZLinkRelocationCapacityFence fence,
            CancellationToken cancellationToken)
    {
        var stored = await ReadRecordAsync<RelocationRecordState>(
                RelocationKey(fence),
                cancellationToken)
            .ConfigureAwait(false);
        if (stored is not null
            && stored.Record.Request == request
            && stored.Record.Status == RelocationStatus.Reserved
            && stored.Record.TargetAuthorityOwnerGeneration > 0)
            return new ZLinkRelocationCapacityReserveResult
                .AlreadyReserved(fence)
            {
                TargetAuthorityOwnerGeneration =
                        stored.Record.TargetAuthorityOwnerGeneration
            };
        return new ZLinkRelocationCapacityReserveResult.Conflict(
            await ReadAuthorityAsync(request.Key, cancellationToken)
                .ConfigureAwait(false));
    }

    public async ValueTask<ZLinkAggregatePrepareResult>
        PrepareAggregateAsync(
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken = default)
    {
        ValidateAggregate(request);
        request = CanonicalizeAggregateRequest(request);
        await EnsureAggregateStagingRecoveryAsync(cancellationToken)
            .ConfigureAwait(false);
        return await PrepareAggregateCoreAsync(
                request,
                0,
                DateTimeOffset.UtcNow + CounterRetryWindow,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkAggregatePrepareResult>
        PrepareAggregateCoreAsync(
            ZLinkAggregatePrepareRequest request,
            int counterRetry,
            DateTimeOffset retryDeadline,
            CancellationToken cancellationToken)
    {
        ValidateAggregate(request);
        var fence = new ZLinkAggregateFence(
            request.AggregateId,
            request.AggregateGeneration);
        var key = AggregateKey(fence);
        var requestFingerprint = AggregateRequestFingerprint(request);
        var header = CreateAggregateHeader(request);
        var inventory = BuildAggregateInventory(request);
        var existing = await ReadRecordAsync<AggregateRecord>(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        if (existing is not null
            && existing.Record.Status != AggregateStatus.Staging)
            return await ReconcileExistingAggregateAsync(
                    fence,
                    existing.Record,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        if (existing is not null
            && (!AggregateHeaderEquals(existing.Record.Header, header)
                || !CryptographicOperations.FixedTimeEquals(
                    existing.Record.RequestFingerprint,
                    requestFingerprint)
                || !AggregateInventoryRootEquals(
                    existing.Record.Inventory,
                    inventory.Root)))
            return new ZLinkAggregatePrepareResult.Conflict();
        var staging = existing;

        var target = await ReadEligibleTargetAsync(
                request.TargetDescriptor,
                request.TargetDescriptorLifecycleGeneration,
                request.TargetOwner,
                null,
                null,
                cancellationToken,
                allowPreparingTarget: request.AllowPreparingTarget)
            .ConfigureAwait(false);
        if (target is null)
            return new ZLinkAggregatePrepareResult.Conflict();
        var authorities = new StoredAuthority?[request.Participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, request.Participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                authorities[index] = await ReadAuthorityRecordAsync(
                        request.Participants[index].Key,
                        token)
                    .ConfigureAwait(false);
            }).ConfigureAwait(false);
        for (var index = 0; index < request.Participants.Count; index++)
        {
            var participant = request.Participants[index];
            var authority = authorities[index];
            var ownsExistingFence = authority is not null
                                    && authority.Meta.AggregateFence == fence
                                    && authority.Meta.AggregateParticipantIndex
                                    == index
                                    && string.Equals(
                                        authority.Meta
                                            .AggregateExpectedStoreVersion,
                                        participant.ExpectedStoreVersion,
                                        StringComparison.Ordinal);
            if (authority is null
                || !ownsExistingFence
                && (authority.Version.Value
                    != participant.ExpectedStoreVersion
                    || authority.Meta.AggregateFence is not null)
                || participant.OwnerTransition
                    == ZLinkAuthorityGenerationTransition.NewOwner
                && !IsEligible(
                    target.Descriptor,
                    authority.Snapshot.Allocation))
                return new ZLinkAggregatePrepareResult.Conflict();
        }
        var capacity = await ReadCapacityAsync(
                request.TargetDescriptor,
                request.TargetDescriptorLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        if (!HasCapacity(target.Descriptor, capacity.Record, request.Capacity))
            return new ZLinkAggregatePrepareResult.Conflict();
        var authorityCounter =
            await ReadAuthorityOwnerGenerationCounterAsync(cancellationToken)
                .ConfigureAwait(false);
        var newOwnerCount = request.Participants.Count(static participant =>
            participant.OwnerTransition
            == ZLinkAuthorityGenerationTransition.NewOwner);
        var authorityHighWater = authorities
            .Select(static authority => authority!)
            .Select(static authority =>
                authority.Meta.AuthorityOwnerGeneration)
            .Append(authorityCounter.Value)
            .Max();
        if (newOwnerCount > 0
            && authorityHighWater
            > (ulong)long.MaxValue - (ulong)newOwnerCount)
            return new ZLinkAggregatePrepareResult.GenerationExhausted();
        var nextIssuedGeneration = authorityHighWater;
        var targetAuthorityOwnerGenerations =
            new Dictionary<ZLinkAuthorityKey, ulong>(
                request.Participants.Count);
        for (var index = 0; index < request.Participants.Count; index++)
        {
            var participant = request.Participants[index];
            var currentGeneration =
                authorities[index]!.Meta.AuthorityOwnerGeneration;
            targetAuthorityOwnerGenerations[participant.Key] =
                participant.OwnerTransition
                == ZLinkAuthorityGenerationTransition.NewOwner
                    ? checked(++nextIssuedGeneration)
                    : currentGeneration;
        }
        try
        {
            staging ??= await ClaimAggregateStagingAsync(
                key,
                header,
                request.Participants.Count,
                requestFingerprint,
                inventory.Root,
                cancellationToken)
            .ConfigureAwait(false);
        if (staging is null)
        {
            var raced = await ReadRecordAsync<AggregateRecord>(
                    key,
                    cancellationToken)
                .ConfigureAwait(false);
            if (raced is null)
                return new ZLinkAggregatePrepareResult.Conflict();
            if (raced.Record.Status != AggregateStatus.Staging)
                return await ReconcileExistingAggregateAsync(
                        fence,
                        raced.Record,
                        request,
                        cancellationToken)
                    .ConfigureAwait(false);
            if (!AggregateHeaderEquals(raced.Record.Header, header)
                || !CryptographicOperations.FixedTimeEquals(
                    raced.Record.RequestFingerprint,
                    requestFingerprint)
                || !AggregateInventoryRootEquals(
                    raced.Record.Inventory,
                    inventory.Root))
                return new ZLinkAggregatePrepareResult.Conflict();
            staging = raced;
        }
            await StageAggregateInventoryAsync(
                fence,
                inventory,
                cancellationToken)
            .ConfigureAwait(false);
        var stagedParticipants = request.Participants
            .Select((participant, index) => new AggregateParticipantRecord(
                index,
                participant.Key.Value,
                participant.ExpectedStoreVersion,
                participant.OwnerTransition,
                Sha256(participant.AuthorityPayload),
                Sha256(participant.MembershipMutation),
                0,
                requestFingerprint))
            .ToArray();
        if (!await StageAggregateParticipantsAsync(
                fence,
                request.Participants,
                stagedParticipants,
                cancellationToken)
            .ConfigureAwait(false))
        {
            await AbortAggregateStagingAsync(
                    fence,
                    staging,
                    cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkAggregatePrepareResult.Conflict();
        }
        await UpdateAggregateParticipantGenerationsAsync(
                fence,
                stagedParticipants,
                targetAuthorityOwnerGenerations,
                cancellationToken)
            .ConfigureAwait(false);
        if (!await InstallAggregateParticipantFencesAsync(
                fence,
                request,
                authorities,
                cancellationToken)
            .ConfigureAwait(false))
        {
            await AbortAggregateStagingAsync(
                    fence,
                    staging,
                    cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkAggregatePrepareResult.Conflict();
        }
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacityVector(
            nextCapacity,
            request.Capacity,
            pendingDelta: 1);
        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(key, staging.Version),
            target.DescriptorCondition,
            target.OwnerCondition,
            capacity.Condition,
            authorityCounter.Condition
        };
        var mutations = new List<ZLinkStoreMutation>
        {
            new ZLinkStoreMutation.Put(
                key,
                Encode(staging.Record with
                {
                    Status = AggregateStatus.Prepared
                }),
                null),
            new ZLinkStoreMutation.Put(
                capacity.Key,
                Encode(nextCapacity),
                null),
            new ZLinkStoreMutation.Put(
                AuthorityOwnerGenerationCounterKey(),
                Encode(new AuthorityOwnerGenerationCounter(
                    nextIssuedGeneration)),
                null)
        };
        ZLinkStoreWriteResult result;
        try
        {
            result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(conditions, mutations),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            var reconciled =
                await ReadRecordForReconciliationAsync<AggregateRecord>(
                        key)
                .ConfigureAwait(false);
            if (reconciled is not null
                && reconciled.Record.Status
                is AggregateStatus.Prepared or AggregateStatus.Committed)
                return await ReconcileExistingAggregateForAmbiguousAsync(
                        fence,
                        reconciled.Record,
                        request)
                    .ConfigureAwait(false);
            throw;
        }
        if (result is ZLinkStoreWriteResult.Conflict
            && counterRetry < CounterRetryLimit
            && DateTimeOffset.UtcNow < retryDeadline)
        {
            var terminal = await ReadRecordAsync<AggregateRecord>(
                    key,
                    cancellationToken)
                .ConfigureAwait(false);
            if (terminal is null
                || terminal.Record.Status == AggregateStatus.Staging)
            {
                var unchanged = true;
                foreach (var participant in request.Participants)
                {
                    var authority = await ReadAuthorityRecordAsync(
                            participant.Key,
                            cancellationToken)
                        .ConfigureAwait(false);
                    if (authority is null
                        || !(authority.Meta.AggregateFence == fence
                             && string.Equals(
                                 authority.Meta
                                     .AggregateExpectedStoreVersion,
                                 participant.ExpectedStoreVersion,
                                 StringComparison.Ordinal)))
                    {
                        unchanged = false;
                        break;
                    }
                }
                if (unchanged)
                {
                    await DelayCounterRetryAsync(
                            counterRetry,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                    return await PrepareAggregateCoreAsync(
                            request,
                            counterRetry + 1,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
        }
        if (result is ZLinkStoreWriteResult.Applied)
            return new ZLinkAggregatePrepareResult.Prepared(fence)
            {
                TargetAuthorityOwnerGenerations =
                    targetAuthorityOwnerGenerations
            };
        var reconciliation = await ReconcileAggregatePrepareAsync(
                key,
                fence,
                request,
                cancellationToken)
            .ConfigureAwait(false);
        if (reconciliation is ZLinkAggregatePrepareResult.Conflict)
        {
            var stillStaging = await ReadRecordAsync<AggregateRecord>(
                    key,
                    cancellationToken)
                .ConfigureAwait(false);
            if (stillStaging?.Record.Status == AggregateStatus.Staging)
                await AbortAggregateStagingAsync(
                        fence,
                        stillStaging,
                        cancellationToken)
                    .ConfigureAwait(false);
        }
            return reconciliation;
        }
        catch
        {
            var recovered =
                await ReconcileOrAbortClaimedAggregatePrepareAsync(
                        fence,
                        header,
                        requestFingerprint,
                        inventory.Root,
                        request)
                    .ConfigureAwait(false);
            if (recovered is not null)
                return recovered;
            throw;
        }
    }

    public async ValueTask<ZLinkAggregateCommitResult>
        CommitAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
    {
        var retryDeadline = DateTimeOffset.UtcNow + CounterRetryWindow;
        return await CommitAggregateCoreAsync(
                fence,
                retryAttempt: 0,
                retryDeadline,
                cancellationToken,
                allowPreparingTarget: false)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkAggregateCommitResult>
        CommitAggregateForRecoveryAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
    {
        var retryDeadline = DateTimeOffset.UtcNow + CounterRetryWindow;
        return await CommitAggregateCoreAsync(
                fence,
                retryAttempt: 0,
                retryDeadline,
                cancellationToken,
                allowPreparingTarget: true)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkAggregateCommitResult>
        CommitAggregateCoreAsync(
            ZLinkAggregateFence fence,
            int retryAttempt,
            DateTimeOffset retryDeadline,
            CancellationToken cancellationToken,
            bool allowPreparingTarget)
    {
        var key = AggregateKey(fence);
        var aggregate = await ReadRecordAsync<AggregateRecord>(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        if (aggregate is null
            || aggregate.Record.Status == AggregateStatus.Aborted)
            return ZLinkAggregateCommitResult.Stale;
        if (aggregate.Record.Status == AggregateStatus.Staging)
            return ZLinkAggregateCommitResult.Stale;
        if (aggregate.Record.Status == AggregateStatus.Committed)
        {
            await NormalizeCommittedAggregateAsync(
                    fence,
                    aggregate.Record,
                    cancellationToken)
                .ConfigureAwait(false);
            return ZLinkAggregateCommitResult.AlreadyCommitted;
        }
        var participants = await ReadAggregateParticipantsAsync(
                fence,
                aggregate.Record,
                cancellationToken)
            .ConfigureAwait(false);
        var request = RehydrateAggregateRequest(
            aggregate.Record,
            participants);
        var target = await ReadEligibleTargetAsync(
                request.TargetDescriptor,
                request.TargetDescriptorLifecycleGeneration,
                request.TargetOwner,
                null,
                null,
                cancellationToken,
                allowPreparingTarget: allowPreparingTarget)
            .ConfigureAwait(false);
        if (target is null) return ZLinkAggregateCommitResult.Stale;
        var authorities = new StoredAuthority?[request.Participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, request.Participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                authorities[index] = await ReadAuthorityRecordAsync(
                        request.Participants[index].Key,
                        knownMeta: null,
                        projectCommittedAggregate: false,
                        token)
                    .ConfigureAwait(false);
            }).ConfigureAwait(false);
        if (authorities.Any(authority =>
                authority is null
                || authority.Meta.AggregateFence != fence))
            return ZLinkAggregateCommitResult.Stale;
        var capacity = await ReadCapacityAsync(
                request.TargetDescriptor,
                request.TargetDescriptorLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var capacityRecords = new Dictionary<ZLinkStoreKey, StoredCapacity>();
        capacityRecords[capacity.Key] = capacity;
        var sourceAllocations = authorities
            .Select(static authority => authority!.Snapshot.Allocation)
            .DistinctBy(static allocation => (
                allocation.Descriptor,
                allocation.DescriptorLifecycleGeneration))
            .ToArray();
        var sourceCapacities = new StoredCapacity[sourceAllocations.Length];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, sourceAllocations.Length),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                var allocation = sourceAllocations[index];
                sourceCapacities[index] = await ReadCapacityAsync(
                        allocation.Descriptor,
                        allocation.DescriptorLifecycleGeneration,
                        token)
                    .ConfigureAwait(false);
            }).ConfigureAwait(false);
        foreach (var source in sourceCapacities)
            capacityRecords[source.Key] = source;
        var updatedCapacity = capacityRecords.ToDictionary(
            static pair => pair.Key,
            static pair => pair.Value.Record.Clone());
        ApplyCapacityVector(
            updatedCapacity[capacity.Key],
            request.Capacity,
            pendingDelta: -1);

        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(key, aggregate.Version),
            target.DescriptorCondition,
            target.OwnerCondition
        };
        var mutations = new List<ZLinkStoreMutation>();
        foreach (var pair in capacityRecords)
        {
            AddCondition(conditions, pair.Value.Condition);
        }
        var generations = new GenerationState?[request.Participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, request.Participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                if (request.Participants[index].OwnerTransition
                    == ZLinkAuthorityGenerationTransition.NewOwner)
                    generations[index] = await ReadGenerationAsync(
                            request.Participants[index].Key,
                            token)
                        .ConfigureAwait(false);
            }).ConfigureAwait(false);
        for (var index = 0; index < request.Participants.Count; index++)
        {
            var participant = request.Participants[index];
            var authority = authorities[index]!;
            var changesOwner = participant.OwnerTransition
                               == ZLinkAuthorityGenerationTransition.NewOwner;
            if (changesOwner)
            {
                var generation = generations[index]!;
                ulong nextAuthorityOwnerGeneration;
                if (generation.ObjectGeneration
                        != authority.Meta.ObjectGeneration
                    || generation.AuthorityOwnerGeneration
                    != authority.Meta.AuthorityOwnerGeneration)
                    return ZLinkAggregateCommitResult.Stale;
                nextAuthorityOwnerGeneration =
                    participants[index].Metadata
                        .TargetAuthorityOwnerGeneration;
                if (nextAuthorityOwnerGeneration == 0
                    || nextAuthorityOwnerGeneration
                       <= generation.AuthorityOwnerGeneration
                    || nextAuthorityOwnerGeneration > long.MaxValue)
                    return ZLinkAggregateCommitResult.Stale;
                ApplyCapacity(
                    updatedCapacity[CapacityKey(
                        authority.Snapshot.Allocation.Descriptor,
                        authority.Snapshot.Allocation
                            .DescriptorLifecycleGeneration)],
                    authority.Snapshot.Allocation,
                    activeDelta: -1);
                var moved = authority.Snapshot.Allocation with
                {
                    Descriptor = request.TargetDescriptor,
                    DescriptorLifecycleGeneration =
                        request.TargetDescriptorLifecycleGeneration
                };
                ApplyCapacity(
                    updatedCapacity[capacity.Key],
                    moved,
                    activeDelta: 1);
            }
        }
        foreach (var pair in updatedCapacity)
            mutations.Add(new ZLinkStoreMutation.Put(
                pair.Key,
                Encode(pair.Value),
                null));
        mutations.Add(new ZLinkStoreMutation.Put(
            key,
            Encode(aggregate.Record with { Status = AggregateStatus.Committed }),
            null));
        ZLinkStoreWriteResult result;
        try
        {
            result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(conditions, mutations),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            var reconciled = await ReadRecordForReconciliationAsync<
                    AggregateRecord>(key)
                .ConfigureAwait(false);
            if (reconciled?.Record.Status != AggregateStatus.Committed)
                throw;
            await NormalizeCommittedAggregateForAmbiguousAsync(
                    fence,
                    reconciled.Record)
                .ConfigureAwait(false);
            return ZLinkAggregateCommitResult.AlreadyCommitted;
        }
        if (result is not ZLinkStoreWriteResult.Applied)
        {
            var reconciled = await ReadRecordAsync<AggregateRecord>(
                    key,
                    cancellationToken)
                .ConfigureAwait(false);
            if (reconciled?.Record.Status == AggregateStatus.Committed)
            {
                await NormalizeCommittedAggregateAsync(
                        fence,
                        reconciled.Record,
                        cancellationToken)
                    .ConfigureAwait(false);
                return ZLinkAggregateCommitResult.AlreadyCommitted;
            }
            if (reconciled?.Record.Status == AggregateStatus.Prepared
                && retryAttempt < CounterRetryLimit
                && DateTimeOffset.UtcNow < retryDeadline)
            {
                await DelayCounterRetryAsync(
                        retryAttempt,
                        retryDeadline,
                        cancellationToken)
                    .ConfigureAwait(false);
                return await CommitAggregateCoreAsync(
                    fence,
                    retryAttempt + 1,
                    retryDeadline,
                    cancellationToken,
                    allowPreparingTarget)
                    .ConfigureAwait(false);
            }
            return ZLinkAggregateCommitResult.Stale;
        }
        await NormalizeCommittedAggregateAsync(
                fence,
                aggregate.Record with { Status = AggregateStatus.Committed },
                cancellationToken)
            .ConfigureAwait(false);
        return ZLinkAggregateCommitResult.Committed;
    }

    private async ValueTask NormalizeCommittedAggregateAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken)
    {
        if (aggregate.Status != AggregateStatus.Committed)
            throw new ZLinkRelocationDataLostException(
                $"Aggregate '{fence.AggregateId:N}' is not a committed authority publication.");
        if (aggregate.ParticipantsNormalized)
        {
            await CleanupAggregateParticipantsAsync(
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var storedParticipants = await ReadAggregateParticipantsAsync(
                fence,
                aggregate,
                cancellationToken)
            .ConfigureAwait(false);
        var request = RehydrateAggregateRequest(aggregate, storedParticipants);
        await Parallel.ForEachAsync(
            storedParticipants,
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (storedParticipant, token) =>
        {
            var participant = request.Participants[storedParticipant.Metadata.Index];
            var normalized = false;
            for (var attempt = 0; attempt < 8; attempt++)
            {
                var authority = await ReadAuthorityRecordAsync(
                        participant.Key,
                        knownMeta: null,
                        projectCommittedAggregate: false,
                        token)
                    .ConfigureAwait(false)
                    ?? throw new ZLinkRelocationDataLostException(
                        $"Committed aggregate authority '{participant.Key.Value}' is missing.");
                if (authority.Meta.AggregateFence is null)
                {
                    if (!IsNormalizedAggregateParticipant(
                            authority,
                            participant,
                            aggregate,
                            storedParticipant.Metadata))
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate authority '{participant.Key.Value}' changed during normalization.");
                    normalized = true;
                    break;
                }
                if (authority.Meta.AggregateFence != fence)
                    throw new ZLinkRelocationDataLostException(
                        $"Committed aggregate authority '{participant.Key.Value}' has a different fence.");

                var changesOwner = participant.OwnerTransition
                                   == ZLinkAuthorityGenerationTransition.NewOwner;
                var nextGeneration =
                    authority.Meta.AuthorityOwnerGeneration;
                var conditions = new List<ZLinkStoreCondition>
                {
                    new ZLinkStoreCondition.Version(
                        AuthorityMetaKey(participant.Key),
                        authority.Version)
                };
                var mutations = new List<ZLinkStoreMutation>();
                if (changesOwner)
                {
                    var generation = await ReadGenerationAsync(
                            participant.Key,
                            token)
                        .ConfigureAwait(false);
                    if (generation.ObjectGeneration
                            != authority.Meta.ObjectGeneration
                        || generation.AuthorityOwnerGeneration
                        != authority.Meta.AuthorityOwnerGeneration
                        || (nextGeneration =
                                storedParticipant.Metadata
                                    .TargetAuthorityOwnerGeneration) == 0
                        || nextGeneration
                           <= generation.AuthorityOwnerGeneration
                        || nextGeneration > long.MaxValue)
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate authority '{participant.Key.Value}' lost its generation fence.");
                    conditions.Add(generation.Condition);
                    mutations.Add(new ZLinkStoreMutation.Put(
                        GenerationKey(participant.Key),
                        Encode(new GenerationRecord(
                            generation.ObjectGeneration,
                            nextGeneration)),
                        null));
                }

                var allocation = changesOwner
                    ? authority.Meta.Allocation with
                    {
                        Descriptor = request.TargetDescriptor,
                        DescriptorLifecycleGeneration =
                            request.TargetDescriptorLifecycleGeneration
                    }
                    : authority.Meta.Allocation;
                mutations.Add(new ZLinkStoreMutation.Put(
                    AuthorityMetaKey(participant.Key),
                    Encode(authority.Meta with
                    {
                        PayloadSha256 =
                            Sha256(participant.AuthorityPayload),
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
                        AggregateExpectedStoreVersion = null
                    }),
                    null));
                mutations.Add(new ZLinkStoreMutation.Put(
                    AuthorityPayloadKey(participant.Key),
                    participant.AuthorityPayload.ToArray(),
                    null));
                var result = await provider.WriteAsync(
                        new ZLinkStoreWriteRequest(conditions, mutations),
                        token)
                    .ConfigureAwait(false);
                if (result is ZLinkStoreWriteResult.Applied)
                {
                    normalized = true;
                    break;
                }
            }
            if (!normalized)
                throw new IOException(
                    $"Committed aggregate authority '{participant.Key.Value}' could not be normalized.");
        }).ConfigureAwait(false);
        await MarkAggregateParticipantsNormalizedAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
        await CleanupAggregateParticipantsAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask MarkAggregateParticipantsNormalizedAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken)
    {
        var key = AggregateKey(fence);
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var aggregate = await ReadRecordAsync<AggregateRecord>(
                    key,
                cancellationToken)
            .ConfigureAwait(false)
                ?? throw new ZLinkRelocationDataLostException(
                    $"Committed aggregate '{fence.AggregateId:N}' is missing.");
            if (aggregate.Record.ParticipantsNormalized) return;
            if (aggregate.Record.Status != AggregateStatus.Committed)
                throw new ZLinkRelocationDataLostException(
                    $"Aggregate '{fence.AggregateId:N}' is not committed.");
            var result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Version(key, aggregate.Version)],
                        [new ZLinkStoreMutation.Put(
                            key,
                            Encode(aggregate.Record with
                            {
                                ParticipantsNormalized = true
                            }),
                            null)]),
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Applied) return;
        }
        throw new IOException(
            $"Aggregate '{fence.AggregateId:N}' normalization marker could not be stored.");
    }

    private async ValueTask CleanupAggregateParticipantsAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken)
    {
        var key = AggregateKey(fence);
        var aggregate = await ReadRecordAsync<AggregateRecord>(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        if (aggregate is null || aggregate.Record.ParticipantsCleaned) return;
        if (aggregate.Record.Status == AggregateStatus.Committed
            && !aggregate.Record.ParticipantsNormalized)
            return;
        if (aggregate.Record.Status is not (
                AggregateStatus.Committed or AggregateStatus.Aborted))
            return;

        // The inventory is the authority for participant identity and index.
        // Validate it before deleting any row so corruption cannot turn a
        // recoverable aggregate into an unrecoverable partial cleanup.
        var inventory = await ReadAndValidateAggregateInventoryAsync(
                fence,
                aggregate.Record,
                cancellationToken)
            .ConfigureAwait(false);
        await DeleteAggregateParticipantRowsAsync(
                fence,
                inventory,
                payloadRows: true,
                cancellationToken)
            .ConfigureAwait(false);
        await DeleteAggregateParticipantRowsAsync(
                fence,
                inventory,
                payloadRows: false,
                cancellationToken)
            .ConfigureAwait(false);
        await DeleteAggregateInventoryPagesAsync(
                fence,
                aggregate.Record.Inventory,
                cancellationToken)
            .ConfigureAwait(false);

        for (var attempt = 0; attempt < 8; attempt++)
        {
            aggregate = await ReadRecordAsync<AggregateRecord>(
                    key,
                    cancellationToken)
                .ConfigureAwait(false);
            if (aggregate is null || aggregate.Record.ParticipantsCleaned)
                return;
            var result = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Version(key, aggregate.Version)],
                        [new ZLinkStoreMutation.Put(
                            key,
                            Encode(aggregate.Record with
                            {
                                ParticipantsCleaned = true
                            }),
                            null)]),
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkStoreWriteResult.Applied) return;
        }
        throw new IOException(
            $"Aggregate '{fence.AggregateId:N}' cleanup marker could not be stored.");
    }

    private async ValueTask DeleteAggregateParticipantRowsAsync(
        ZLinkAggregateFence fence,
        IReadOnlyList<AggregateInventoryLeafEntry> inventory,
        bool payloadRows,
        CancellationToken cancellationToken)
    {
        const int maximumDeletesPerBatch = 1024;
        for (var offset = 0;
             offset < inventory.Count;
             offset += maximumDeletesPerBatch)
        {
            var count = Math.Min(
                maximumDeletesPerBatch,
                inventory.Count - offset);
            var mutations = inventory
                .Skip(offset)
                .Take(count)
                .Select(entry => (ZLinkStoreMutation)
                    new ZLinkStoreMutation.Delete(
                        payloadRows
                            ? AggregateParticipantPayloadKey(
                                fence,
                                checked((int)entry.Index))
                            : AggregateParticipantMetaKey(
                                fence,
                                checked((int)entry.Index))))
                .ToArray();
            await provider.WriteAsync(
                    new ZLinkStoreWriteRequest([], mutations),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask DeleteAggregateInventoryPagesAsync(
        ZLinkAggregateFence fence,
        AggregateInventoryRoot inventory,
        CancellationToken cancellationToken)
    {
        const int maximumDeletesPerBatch = 1024;
        if (inventory.PageCountsByLevel is null)
            throw InventoryDataLost(fence, "page counts are missing");
        for (var level = 0;
             level < inventory.PageCountsByLevel.Count;
             level++)
        {
            var pageCount = inventory.PageCountsByLevel[level];
            if (pageCount < 1 || pageCount > inventory.TotalCount)
                throw InventoryDataLost(
                    fence,
                    $"level {level} page count is invalid");
            for (var offset = 0;
                 offset < pageCount;
                 offset += maximumDeletesPerBatch)
            {
                var count = Math.Min(
                    maximumDeletesPerBatch,
                    pageCount - offset);
                var mutations = Enumerable.Range(offset, count)
                    .Select(index => (ZLinkStoreMutation)
                        new ZLinkStoreMutation.Delete(
                            AggregateInventoryPageKey(
                                fence,
                                level,
                                index)))
                    .ToArray();
                await provider.WriteAsync(
                        new ZLinkStoreWriteRequest([], mutations),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }
    }

    private static bool IsNormalizedAggregateParticipant(
        StoredAuthority authority,
        ZLinkAggregateParticipant participant,
        AggregateRecord aggregate,
        AggregateParticipantRecord storedParticipant)
    {
        var changesOwner = participant.OwnerTransition
                           == ZLinkAuthorityGenerationTransition.NewOwner;
        if (!CryptographicOperations.FixedTimeEquals(
                authority.Snapshot.Payload.Span,
                participant.AuthorityPayload.Span))
            return false;
        if (!changesOwner)
            return true;
        var generation = storedParticipant.TargetAuthorityOwnerGeneration;
        return generation > 0
               && authority.Meta.AuthorityOwnerGeneration == generation
               && authority.Meta.OwnerId == aggregate.Header.TargetOwner.OwnerId
               && authority.Meta.OwnerLeaseGeneration
               == aggregate.Header.TargetOwner.LeaseGeneration
               && authority.Meta.Allocation.Descriptor
               == aggregate.Header.TargetDescriptor
               && authority.Meta.Allocation.DescriptorLifecycleGeneration
               == aggregate.Header.TargetDescriptorLifecycleGeneration;
    }

    private async ValueTask<StoredRecord<AggregateRecord>?>
        ClaimAggregateStagingAsync(
            ZLinkStoreKey key,
            AggregateHeader header,
            int participantCount,
            byte[] requestFingerprint,
            AggregateInventoryRoot inventory,
            CancellationToken cancellationToken)
    {
        var storeNow = await ReadStoreNowAsync(cancellationToken)
            .ConfigureAwait(false);
        var staged = new AggregateRecord(
            header,
            participantCount,
            requestFingerprint,
            inventory,
            AggregateStatus.Staging,
            storeNow,
            false,
            false);
        try
        {
            var claimed = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Missing(key)],
                        [new ZLinkStoreMutation.Put(
                            key,
                            Encode(staged),
                            null)]),
                    cancellationToken)
                .ConfigureAwait(false);
            return claimed is ZLinkStoreWriteResult.Applied applied
                ? new StoredRecord<AggregateRecord>(
                    staged,
                    applied.PutVersions[key],
                    applied.StoreNow)
                : null;
        }
        catch
        {
            var reconciled = await ReadRecordForReconciliationAsync<
                    AggregateRecord>(key)
                .ConfigureAwait(false);
            if (reconciled is not null
                && reconciled.Record.Status == AggregateStatus.Staging
                && AggregateHeaderEquals(reconciled.Record.Header, header)
                && CryptographicOperations.FixedTimeEquals(
                    reconciled.Record.RequestFingerprint,
                    requestFingerprint)
                && AggregateInventoryRootEquals(
                    reconciled.Record.Inventory,
                    inventory))
                return reconciled;
            throw;
        }
    }

    private async ValueTask AbortAggregateStagingAsync(
        ZLinkAggregateFence fence,
        StoredRecord<AggregateRecord> staging,
        CancellationToken cancellationToken)
    {
        if (staging.Record.Status != AggregateStatus.Staging) return;
        var key = AggregateKey(fence);
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(key, staging.Version)],
                    [new ZLinkStoreMutation.Put(
                        key,
                        Encode(staging.Record with
                        {
                            Status = AggregateStatus.Aborted
                        }),
                        null)]),
                cancellationToken)
            .ConfigureAwait(false);
        var aborted = result is ZLinkStoreWriteResult.Applied
            ? staging.Record with { Status = AggregateStatus.Aborted }
            : (await ReadRecordAsync<AggregateRecord>(
                    key,
                    cancellationToken)
                .ConfigureAwait(false))?.Record;
        if (aborted?.Status != AggregateStatus.Aborted)
            return;
        await ReleaseAggregateParticipantFencesAsync(
                fence,
                aborted,
                cancellationToken)
            .ConfigureAwait(false);
        await CleanupAggregateParticipantsAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkAggregatePrepareResult?>
        ReconcileOrAbortClaimedAggregatePrepareAsync(
            ZLinkAggregateFence fence,
            AggregateHeader header,
            byte[] requestFingerprint,
            AggregateInventoryRoot inventory,
            ZLinkAggregatePrepareRequest? request = null)
    {
        using var deadline = new CancellationTokenSource(
            AmbiguousReconciliationTimeout);
        try
        {
            for (var attempt = 0; attempt < 8; attempt++)
            {
                deadline.Token.ThrowIfCancellationRequested();
                var aggregate = await ReadRecordAsync<AggregateRecord>(
                        AggregateKey(fence),
                        deadline.Token)
                    .ConfigureAwait(false);
                if (aggregate is null)
                    return null;
                if (!AggregateHeaderEquals(aggregate.Record.Header, header)
                    || !CryptographicOperations.FixedTimeEquals(
                        aggregate.Record.RequestFingerprint,
                        requestFingerprint)
                    || !AggregateInventoryRootEquals(
                        aggregate.Record.Inventory,
                        inventory))
                    return new ZLinkAggregatePrepareResult.Conflict();

                if (aggregate.Record.Status
                    is AggregateStatus.Prepared
                        or AggregateStatus.Committed)
                {
                    if (request is null)
                        return new ZLinkAggregatePrepareResult.Prepared(fence);
                    return await ReconcileExistingAggregateAsync(
                            fence,
                            aggregate.Record,
                            request,
                            deadline.Token)
                        .ConfigureAwait(false);
                }

                if (aggregate.Record.Status == AggregateStatus.Aborted)
                {
                    try
                    {
                        await ReleaseAggregateParticipantFencesAsync(
                                fence,
                                aggregate.Record,
                                deadline.Token)
                            .ConfigureAwait(false);
                        await CleanupAggregateParticipantsAsync(
                                fence,
                                deadline.Token)
                            .ConfigureAwait(false);
                    }
                    catch (ZLinkRelocationDataLostException)
                    {
                        if (request is null
                            || await HasInstalledAggregateFenceAsync(
                                    fence,
                                    request.Participants,
                                    deadline.Token)
                                .ConfigureAwait(false))
                            throw;

                        // The claim can be aborted before inventory staging.
                        // After every requested authority proves that this
                        // fence was never installed, remove any partially
                        // staged child rows without trusting the missing tree.
                        await CleanupUnfencedAggregateChildrenAsync(
                                fence,
                                deadline.Token)
                            .ConfigureAwait(false);
                    }
                    return new ZLinkAggregatePrepareResult.Conflict();
                }

                var abort = await provider.WriteAsync(
                        new ZLinkStoreWriteRequest(
                            [new ZLinkStoreCondition.Version(
                                AggregateKey(fence),
                                aggregate.Version)],
                            [new ZLinkStoreMutation.Put(
                                AggregateKey(fence),
                                Encode(aggregate.Record with
                                {
                                    Status = AggregateStatus.Aborted
                                }),
                                null)]),
                        deadline.Token)
                    .ConfigureAwait(false);
                if (abort is ZLinkStoreWriteResult.Applied)
                    continue;
            }
        }
        catch (ZLinkRelocationDataLostException)
        {
            throw;
        }
        catch (OperationCanceledException) when (
            deadline.IsCancellationRequested)
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
        CancellationToken cancellationToken)
    {
        var installed = new bool[participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                var authority = await ReadAuthorityRecordAsync(
                        participants[index].Key,
                        knownMeta: null,
                        projectCommittedAggregate: false,
                        token)
                    .ConfigureAwait(false)
                    ?? throw new ZLinkRelocationDataLostException(
                        $"Aggregate '{fence.AggregateId:N}' participant authority '{participants[index].Key.Value}' is missing.");
                installed[index] =
                    authority.Meta.AggregateFence == fence;
            }).ConfigureAwait(false);
        return installed.Any(static value => value);
    }

    private async ValueTask CleanupUnfencedAggregateChildrenAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken)
    {
        var childPrefix = AggregateKey(fence).Value + ":";
        var keys = new List<ZLinkStoreKey>();
        ZLinkStoreScanCursor? cursor = null;
        do
        {
            var scan = await provider.ScanAsync(
                    new ZLinkStoreScanRequest(childPrefix, cursor, 1000),
                    cancellationToken)
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                keys.Clear();
                cursor = null;
                continue;
            }
            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            keys.AddRange(page.Items.Select(static item => item.Key));
            cursor = page.NextCursor;
        } while (cursor is not null);

        const int maximumDeletesPerBatch = 1024;
        for (var offset = 0;
             offset < keys.Count;
             offset += maximumDeletesPerBatch)
        {
            var mutations = keys
                .Skip(offset)
                .Take(maximumDeletesPerBatch)
                .Select(static key =>
                    (ZLinkStoreMutation)new ZLinkStoreMutation.Delete(key))
                .ToArray();
            await provider.WriteAsync(
                    new ZLinkStoreWriteRequest([], mutations),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask ReleaseAggregateParticipantFencesAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken)
    {
        var inventory = await ReadAndValidateAggregateInventoryAsync(
                fence,
                aggregate,
                cancellationToken)
            .ConfigureAwait(false);
        await Parallel.ForEachAsync(
            inventory,
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (entry, token) =>
            {
                var index = checked((int)entry.Index);
                var key = new ZLinkAuthorityKey(entry.Key);
                for (var attempt = 0; attempt < 8; attempt++)
                {
                    var authority = await ReadAuthorityRecordAsync(
                            key,
                            knownMeta: null,
                            projectCommittedAggregate: false,
                            token)
                        .ConfigureAwait(false);
                    if (authority is null)
                        throw new ZLinkRelocationDataLostException(
                            $"Aggregate '{fence.AggregateId:N}' authority '{key.Value}' is missing while releasing its fence.");
                    if (authority.Meta.AggregateFence is null)
                        return;
                    if (authority.Meta.AggregateFence != fence
                        || authority.Meta.AggregateParticipantIndex != index
                        || !string.Equals(
                            authority.Meta.AggregateExpectedStoreVersion,
                            entry.ExpectedStoreVersion,
                            StringComparison.Ordinal))
                        throw new ZLinkRelocationDataLostException(
                            $"Aggregate '{fence.AggregateId:N}' authority '{key.Value}' has a different fence.");
                    var result = await provider.WriteAsync(
                            new ZLinkStoreWriteRequest(
                                [new ZLinkStoreCondition.Version(
                                    AuthorityMetaKey(key),
                                    authority.Version)],
                                [new ZLinkStoreMutation.Put(
                                    AuthorityMetaKey(key),
                                    Encode(authority.Meta with
                                    {
                                        AggregateFence = null,
                                        AggregateParticipantIndex = null,
                                        AggregateExpectedStoreVersion = null
                                    }),
                                    null)]),
                            token)
                        .ConfigureAwait(false);
                    if (result is ZLinkStoreWriteResult.Applied) return;
                }
                throw new IOException(
                    $"Aggregate '{fence.AggregateId:N}' authority '{key.Value}' fence could not be released.");
            }).ConfigureAwait(false);
    }

    private async ValueTask EnsureAggregateStagingRecoveryAsync(
        CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref aggregateStagingRecoveryCompleted) != 0)
            return;
        await aggregateRecoveryGate.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        try
        {
            if (aggregateStagingRecoveryCompleted != 0) return;
            var abandoned = new List<(
                ZLinkAggregateFence Fence,
                StoredRecord<AggregateRecord> Aggregate)>();
            var preparedToAbort = new List<ZLinkAggregateFence>();
            var terminalToFinish = new List<(
                ZLinkAggregateFence Fence,
                AggregateRecord Aggregate)>();
            ZLinkStoreScanCursor? cursor = null;
            var expiredRestarts = 0;
            do
            {
                var scan = await provider.ScanAsync(
                        new ZLinkStoreScanRequest(
                            AggregatePrefix,
                            cursor,
                            256),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (scan is ZLinkStoreScanResult.Expired)
                {
                    if (++expiredRestarts > 8)
                        throw new IOException(
                            "The Location Store aggregate recovery scan expired repeatedly.");
                    abandoned.Clear();
                    preparedToAbort.Clear();
                    terminalToFinish.Clear();
                    cursor = null;
                    continue;
                }
                var page = ((ZLinkStoreScanResult.Page)scan).Value;
                foreach (var pair in page.Items)
                {
                    if (pair.Key.Value.Contains(
                            ":participant:",
                            StringComparison.Ordinal))
                        continue;
                    if (pair.Key.Value.Contains(
                            ":inventory:",
                            StringComparison.Ordinal))
                        continue;
                    AggregateRecord aggregate;
                    try
                    {
                        aggregate = Decode<AggregateRecord>(pair.Value.Bytes);
                    }
                    catch (Exception error) when (
                        error is JsonException
                            or InvalidDataException
                            or NotSupportedException
                            or ArgumentException)
                    {
                        throw new ZLinkRelocationDataLostException(
                            $"Aggregate durable root '{pair.Key.Value}' cannot be decoded ({error.GetType().Name}).");
                    }
                    var fence = new ZLinkAggregateFence(
                        aggregate.Header.AggregateId,
                        aggregate.Header.AggregateGeneration);
                    if (aggregate.Status == AggregateStatus.Committed)
                    {
                        if (!aggregate.ParticipantsCleaned)
                            terminalToFinish.Add((fence, aggregate));
                        continue;
                    }
                    if (aggregate.Status == AggregateStatus.Aborted)
                    {
                        if (!aggregate.ParticipantsCleaned)
                            terminalToFinish.Add((fence, aggregate));
                        continue;
                    }
                    if (aggregate.StagedAt
                        > pair.Value.StoreNow - AggregateStagingRetention
                        || await ReadLiveOwnerAsync(
                                aggregate.Header.TargetOwner,
                                cancellationToken)
                            .ConfigureAwait(false) is not null)
                        continue;
                    if (aggregate.Status == AggregateStatus.Staging)
                        abandoned.Add((
                            fence,
                            new StoredRecord<AggregateRecord>(
                                aggregate,
                                pair.Value.Version,
                                pair.Value.StoreNow)));
                    else if (aggregate.Status == AggregateStatus.Prepared)
                        preparedToAbort.Add(fence);
                }
                cursor = page.NextCursor;
            } while (cursor is not null);
            foreach (var item in abandoned)
                await AbortAggregateStagingAsync(
                        item.Fence,
                        item.Aggregate,
                        cancellationToken)
                    .ConfigureAwait(false);
            foreach (var fence in preparedToAbort)
                await AbortAggregateAsync(fence, cancellationToken)
                    .ConfigureAwait(false);
            foreach (var item in terminalToFinish)
            {
                if (item.Aggregate.Status == AggregateStatus.Committed)
                    await NormalizeCommittedAggregateAsync(
                            item.Fence,
                            item.Aggregate,
                            cancellationToken)
                        .ConfigureAwait(false);
                else
                {
                    await ReleaseAggregateParticipantFencesAsync(
                            item.Fence,
                            item.Aggregate,
                            cancellationToken)
                        .ConfigureAwait(false);
                    await CleanupAggregateParticipantsAsync(
                            item.Fence,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            Volatile.Write(ref aggregateStagingRecoveryCompleted, 1);
        }
        finally
        {
            aggregateRecoveryGate.Release();
        }
    }

    private async ValueTask<bool> StageAggregateParticipantsAsync(
        ZLinkAggregateFence fence,
        IReadOnlyList<ZLinkAggregateParticipant> participants,
        IReadOnlyList<AggregateParticipantRecord> records,
        CancellationToken cancellationToken)
    {
        var exact = new bool[participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                var participant = participants[index];
                var record = records[index];
                var metaKey = AggregateParticipantMetaKey(fence, index);
                var payloadKey = AggregateParticipantPayloadKey(fence, index);
                ZLinkStoreWriteResult result;
                try
                {
                    result = await provider.WriteAsync(
                            new ZLinkStoreWriteRequest(
                            [
                                new ZLinkStoreCondition.Missing(metaKey),
                                new ZLinkStoreCondition.Missing(payloadKey)
                            ],
                            [
                                new ZLinkStoreMutation.Put(
                                    metaKey,
                                    Encode(record),
                                    null),
                                new ZLinkStoreMutation.Put(
                                    payloadKey,
                                    participant.AuthorityPayload.ToArray(),
                                    null)
                            ]),
                            token)
                        .ConfigureAwait(false);
                }
                catch
                {
                    exact[index] =
                        await IsExactStagedParticipantForReconciliationAsync(
                            metaKey,
                            payloadKey,
                            record,
                            participant.AuthorityPayload)
                        .ConfigureAwait(false);
                    if (!exact[index]) throw;
                    return;
                }
                exact[index] = result is ZLinkStoreWriteResult.Applied
                               || await IsExactStagedParticipantAsync(
                                       metaKey,
                                       payloadKey,
                                       record,
                                       participant.AuthorityPayload,
                                       token)
                                   .ConfigureAwait(false);
            }).ConfigureAwait(false);
        return exact.All(static value => value);
    }

    private static AggregateInventoryTree BuildAggregateInventory(
        ZLinkAggregatePrepareRequest request)
    {
        var entries = request.Participants
            .OrderBy(
                static participant => participant.Key.Value,
                StringComparer.Ordinal)
            .Select((participant, index) =>
                new AggregateInventoryLeafEntry(
                    index,
                    participant.Key.Value,
                    participant.ExpectedStoreVersion,
                    participant.OwnerTransition,
                    Sha256(participant.AuthorityPayload),
                    Sha256(participant.MembershipMutation)))
            .ToArray();
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var entry in entries)
            digest.AppendData(EncodeAggregateInventoryLeafEntry(entry));
        var pages = new List<AggregateInventoryPage>();
        var references = PackInventoryLeafPages(entries, pages);
        var level = 0;
        while (references.Count > MaxInventoryPageEntries
               || EncodeAggregateInventoryRoot(new AggregateInventoryRoot(
                       entries.Length,
                       new byte[SHA256.HashSizeInBytes],
                       level,
                       references,
                       []))
                   .Length > MaxInventoryPageBytes)
        {
            level++;
            references = PackInventoryUpperPages(
                level,
                references,
                pages);
        }
        var pageCounts = pages
            .GroupBy(static page => page.Level)
            .OrderBy(static group => group.Key)
            .Select(static group => group.Count())
            .ToArray();
        var root = new AggregateInventoryRoot(
            entries.Length,
            digest.GetHashAndReset(),
            level,
            references,
            pageCounts);
        if (EncodeAggregateInventoryRoot(root).Length > MaxInventoryPageBytes)
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "The aggregate inventory root exceeds 1 MiB.");
        return new AggregateInventoryTree(root, pages);
    }

    private static byte[] EncodeAggregateInventoryRoot(
        AggregateInventoryRoot root)
    {
        using var stream = new MemoryStream();
        WriteInventoryHeader(stream, InventoryRootRecordKind);
        WriteInventoryInt32(stream, root.TotalCount);
        WriteInventoryHash(stream, root.Digest);
        WriteInventoryInt32(stream, root.TopLevel);
        WriteInventoryInt32(stream, root.TopPages.Count);
        foreach (var reference in root.TopPages)
            WriteInventoryReference(stream, reference);
        WriteInventoryInt32(stream, root.PageCountsByLevel.Count);
        foreach (var count in root.PageCountsByLevel)
            WriteInventoryInt32(stream, count);
        return stream.ToArray();
    }

    private static byte[] EncodeAggregateInventoryPage(
        AggregateInventoryPage page)
    {
        using var stream = new MemoryStream();
        WriteInventoryHeader(stream, InventoryPageRecordKind);
        WriteInventoryInt32(stream, page.Level);
        WriteInventoryInt32(stream, page.Index);
        WriteInventoryInt32(stream, page.StartIndex);
        WriteInventoryInt32(stream, page.EntryCount);
        WriteInventoryInt32(stream, page.Entries.Count);
        foreach (var entry in page.Entries)
            WriteInventoryLeafEntry(stream, entry);
        WriteInventoryInt32(stream, page.Children.Count);
        foreach (var child in page.Children)
            WriteInventoryReference(stream, child);
        return stream.ToArray();
    }

    private static byte[] EncodeAggregateInventoryLeafEntry(
        AggregateInventoryLeafEntry entry)
    {
        using var stream = new MemoryStream();
        WriteInventoryHeader(stream, InventoryLeafEntryRecordKind);
        WriteInventoryLeafEntry(stream, entry);
        return stream.ToArray();
    }

    private static AggregateInventoryPage DecodeAggregateInventoryPage(
        ReadOnlyMemory<byte> encoded)
    {
        var reader = new AggregateInventoryReader(encoded.Span);
        reader.ReadHeader(InventoryPageRecordKind);
        var level = reader.ReadInt32();
        var index = reader.ReadInt32();
        var startIndex = reader.ReadInt32();
        var entryCount = reader.ReadInt32();
        var leafCount = reader.ReadBoundedCount(
            MaxInventoryPageEntries);
        var entries = new AggregateInventoryLeafEntry[leafCount];
        for (var entryIndex = 0; entryIndex < leafCount; entryIndex++)
            entries[entryIndex] = reader.ReadLeafEntry();
        var childCount = reader.ReadBoundedCount(
            MaxInventoryPageEntries);
        var children =
            new AggregateInventoryPageReference[childCount];
        for (var childIndex = 0; childIndex < childCount; childIndex++)
            children[childIndex] = reader.ReadReference();
        reader.RequireEnd();
        return new AggregateInventoryPage(
            level,
            index,
            startIndex,
            entryCount,
            entries,
            children);
    }

    private static void WriteInventoryHeader(
        Stream stream,
        byte recordKind)
    {
        Span<byte> header = stackalloc byte[6];
        BinaryPrimitives.WriteUInt32BigEndian(
            header,
            InventoryCodecMagic);
        header[4] = InventoryCodecVersion;
        header[5] = recordKind;
        stream.Write(header);
    }

    private static void WriteInventoryInt32(
        Stream stream,
        int value)
    {
        Span<byte> encoded = stackalloc byte[sizeof(int)];
        BinaryPrimitives.WriteInt32BigEndian(encoded, value);
        stream.Write(encoded);
    }

    private static void WriteInventoryString(
        Stream stream,
        string value)
    {
        var encoded = InventoryUtf8.GetBytes(value);
        WriteInventoryInt32(stream, encoded.Length);
        stream.Write(encoded);
    }

    private static void WriteInventoryHash(
        Stream stream,
        byte[] hash)
    {
        if (hash.Length != SHA256.HashSizeInBytes)
            throw new InvalidDataException(
                "An aggregate inventory SHA-256 value is invalid.");
        stream.Write(hash);
    }

    private static void WriteInventoryLeafEntry(
        Stream stream,
        AggregateInventoryLeafEntry entry)
    {
        WriteInventoryInt32(stream, entry.Index);
        WriteInventoryString(stream, entry.Key);
        WriteInventoryString(stream, entry.ExpectedStoreVersion);
        stream.WriteByte((byte)entry.OwnerTransition);
        WriteInventoryHash(stream, entry.AuthorityPayloadSha256);
        WriteInventoryHash(stream, entry.MembershipMutationSha256);
    }

    private static void WriteInventoryReference(
        Stream stream,
        AggregateInventoryPageReference reference)
    {
        WriteInventoryInt32(stream, reference.Level);
        WriteInventoryInt32(stream, reference.Index);
        WriteInventoryInt32(stream, reference.StartIndex);
        WriteInventoryInt32(stream, reference.EntryCount);
        WriteInventoryHash(stream, reference.Sha256);
    }

    private static IReadOnlyList<AggregateInventoryPageReference>
        PackInventoryLeafPages(
            IReadOnlyList<AggregateInventoryLeafEntry> entries,
            ICollection<AggregateInventoryPage> pages)
    {
        var references = new List<AggregateInventoryPageReference>();
        for (var offset = 0; offset < entries.Count;)
        {
            var count = Math.Min(
                MaxInventoryPageEntries,
                entries.Count - offset);
            AggregateInventoryPage page;
            byte[] encoded;
            do
            {
                page = new AggregateInventoryPage(
                    0,
                    references.Count,
                    offset,
                    count,
                    entries.Skip(offset).Take(count).ToArray(),
                    []);
                encoded = EncodeAggregateInventoryPage(page);
                if (encoded.Length <= MaxInventoryPageBytes) break;
                count /= 2;
            } while (count > 0);
            if (count == 0 || encoded.Length > MaxInventoryPageBytes)
                throw new ArgumentOutOfRangeException(
                    nameof(entries),
                    "An aggregate inventory entry exceeds 1 MiB.");
            pages.Add(page);
            references.Add(InventoryReference(page, encoded));
            offset += count;
        }
        return references;
    }

    private static IReadOnlyList<AggregateInventoryPageReference>
        PackInventoryUpperPages(
            int level,
            IReadOnlyList<AggregateInventoryPageReference> children,
            ICollection<AggregateInventoryPage> pages)
    {
        var references = new List<AggregateInventoryPageReference>();
        for (var offset = 0; offset < children.Count;)
        {
            var count = Math.Min(
                MaxInventoryPageEntries,
                children.Count - offset);
            AggregateInventoryPage page;
            byte[] encoded;
            do
            {
                var selected = children.Skip(offset).Take(count).ToArray();
                page = new AggregateInventoryPage(
                    level,
                    references.Count,
                    selected[0].StartIndex,
                    selected.Sum(static child => child.EntryCount),
                    [],
                    selected);
                encoded = EncodeAggregateInventoryPage(page);
                if (encoded.Length <= MaxInventoryPageBytes) break;
                count /= 2;
            } while (count > 0);
            if (count == 0 || encoded.Length > MaxInventoryPageBytes)
                throw new ArgumentOutOfRangeException(
                    nameof(children),
                    "An aggregate inventory index entry exceeds 1 MiB.");
            pages.Add(page);
            references.Add(InventoryReference(page, encoded));
            offset += count;
        }
        return references;
    }

    private static AggregateInventoryPageReference InventoryReference(
        AggregateInventoryPage page,
        ReadOnlySpan<byte> encoded) =>
        new(
            page.Level,
            page.Index,
            page.StartIndex,
            page.EntryCount,
            SHA256.HashData(encoded));

    private async ValueTask StageAggregateInventoryAsync(
        ZLinkAggregateFence fence,
        AggregateInventoryTree inventory,
        CancellationToken cancellationToken)
    {
        await Parallel.ForEachAsync(
            inventory.Pages,
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (page, token) =>
            {
                var key = AggregateInventoryPageKey(
                    fence,
                    page.Level,
                    page.Index);
                var encoded = EncodeAggregateInventoryPage(page);
                if (encoded.Length > MaxInventoryPageBytes)
                    throw new InvalidDataException(
                        $"Aggregate inventory page {page.Level}:{page.Index} exceeds 1 MiB.");
                var result = await provider.WriteAsync(
                        new ZLinkStoreWriteRequest(
                            [new ZLinkStoreCondition.Missing(key)],
                            [new ZLinkStoreMutation.Put(key, encoded, null)]),
                        token)
                    .ConfigureAwait(false);
                if (result is ZLinkStoreWriteResult.Applied) return;
                var read = await provider.ReadAsync(key, token)
                    .ConfigureAwait(false);
                if (read is not ZLinkStoreReadResult.Found found
                    || !found.Value.Bytes.Span.SequenceEqual(encoded))
                    throw new ZLinkRelocationDataLostException(
                        $"Aggregate inventory page {page.Level}:{page.Index} changed.");
            }).ConfigureAwait(false);
    }

    private static bool AggregateInventoryRootEquals(
        AggregateInventoryRoot left,
        AggregateInventoryRoot right) =>
        EncodeAggregateInventoryRoot(left)
            .AsSpan()
            .SequenceEqual(EncodeAggregateInventoryRoot(right));

    private async ValueTask<AggregateInventoryLeafEntry[]>
        ReadAndValidateAggregateInventoryAsync(
            ZLinkAggregateFence fence,
            AggregateRecord aggregate,
            CancellationToken cancellationToken)
    {
        var root = aggregate.Inventory;
        if (root is null
            || root.TotalCount != aggregate.ParticipantCount
            || root.TotalCount < 1
            || root.Digest is null
            || root.Digest.Length != SHA256.HashSizeInBytes
            || root.TopLevel < 0
            || root.TopLevel >= MaxInventoryTreeLevels
            || root.TopPages is null
            || root.TopPages.Count is < 1 or > MaxInventoryPageEntries
            || root.PageCountsByLevel is null
            || root.PageCountsByLevel.Count != root.TopLevel + 1
            || root.PageCountsByLevel.Any(
                count => count < 1 || count > root.TotalCount)
            || EncodeAggregateInventoryRoot(root).Length
               > MaxInventoryPageBytes)
            throw InventoryDataLost(
                fence,
                "root metadata is invalid");

        var leafPageCount = root.PageCountsByLevel[0];
        if (leafPageCount > root.TotalCount
            || (long)root.TotalCount
               > (long)leafPageCount * MaxInventoryPageEntries)
            throw InventoryDataLost(
                fence,
                "leaf page count cannot contain the declared total");
        for (var level = 1;
             level < root.PageCountsByLevel.Count;
             level++)
        {
            var lowerCount = root.PageCountsByLevel[level - 1];
            var currentCount = root.PageCountsByLevel[level];
            var minimum = ((long)lowerCount
                           + MaxInventoryPageEntries - 1)
                          / MaxInventoryPageEntries;
            if (currentCount < minimum || currentCount > lowerCount)
                throw InventoryDataLost(
                    fence,
                    $"level {level} page count cannot index level {level - 1}");
        }
        if (root.TopPages.Count
            != root.PageCountsByLevel[root.TopLevel])
            throw InventoryDataLost(
                fence,
                "top page count does not match the root");

        var entries = new List<AggregateInventoryLeafEntry>();
        var observedPageCounts = new int[root.TopLevel + 1];
        var visited = new HashSet<(int Level, int Index)>();
        long expectedStart = 0;
        foreach (var reference in root.TopPages)
        {
            if (reference.Level != root.TopLevel
                || reference.StartIndex != expectedStart)
                throw InventoryDataLost(
                    fence,
                    "top page references are reordered");
            await ReadInventoryPageAsync(reference).ConfigureAwait(false);
            expectedStart += reference.EntryCount;
        }
        if (expectedStart != root.TotalCount
            || entries.Count != root.TotalCount
            || !observedPageCounts.SequenceEqual(root.PageCountsByLevel))
            throw InventoryDataLost(
                fence,
                "page counts do not match the root");
        for (var level = 0; level < observedPageCounts.Length; level++)
        {
            for (var index = 0; index < observedPageCounts[level]; index++)
            {
                if (!visited.Contains((level, index)))
                    throw InventoryDataLost(
                        fence,
                        $"level {level} page indexes are not contiguous");
            }
        }

        using var digest = IncrementalHash.CreateHash(
            HashAlgorithmName.SHA256);
        for (var index = 0; index < entries.Count; index++)
        {
            var entry = entries[index];
            if (entry.Index != index
                || string.IsNullOrEmpty(entry.Key)
                || string.IsNullOrEmpty(entry.ExpectedStoreVersion)
                || entry.AuthorityPayloadSha256 is null
                || entry.AuthorityPayloadSha256.Length
                   != SHA256.HashSizeInBytes
                || entry.MembershipMutationSha256 is null
                || entry.MembershipMutationSha256.Length
                   != SHA256.HashSizeInBytes)
                throw InventoryDataLost(
                    fence,
                    $"leaf entry {index} is invalid or reordered");
            digest.AppendData(EncodeAggregateInventoryLeafEntry(entry));
        }
        if (!CryptographicOperations.FixedTimeEquals(
                digest.GetHashAndReset(),
                root.Digest))
            throw InventoryDataLost(fence, "digest does not match");
        return entries.ToArray();

        async ValueTask ReadInventoryPageAsync(
            AggregateInventoryPageReference reference)
        {
            if (reference.Level < 0
                || reference.Level > root.TopLevel
                || reference.Index < 0
                || reference.StartIndex < 0
                || reference.EntryCount < 1
                || reference.Sha256 is null
                || reference.Sha256.Length != SHA256.HashSizeInBytes
                || !visited.Add((reference.Level, reference.Index)))
                throw InventoryDataLost(
                    fence,
                    "a page reference is invalid or duplicated");

            var read = await provider.ReadAsync(
                    AggregateInventoryPageKey(
                        fence,
                        reference.Level,
                        reference.Index),
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkStoreReadResult.Found found)
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} is missing");
            if (found.Value.Bytes.Length > MaxInventoryPageBytes
                || !CryptographicOperations.FixedTimeEquals(
                    SHA256.HashData(found.Value.Bytes.Span),
                    reference.Sha256))
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} checksum is invalid");

            AggregateInventoryPage page;
            try
            {
                page = DecodeAggregateInventoryPage(found.Value.Bytes);
            }
            catch (Exception error) when (
                error is InvalidDataException
                    or DecoderFallbackException
                    or JsonException
                    or NotSupportedException
                    or ArgumentException)
            {
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} cannot be decoded ({error.GetType().Name})");
            }
            if (page.Level != reference.Level
                || page.Index != reference.Index
                || page.StartIndex != reference.StartIndex
                || page.EntryCount != reference.EntryCount)
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} metadata changed");
            observedPageCounts[page.Level]++;

            if (page.Level == 0)
            {
                if (page.Entries is null
                    || page.Entries.Count is < 1
                        or > MaxInventoryPageEntries
                    || page.Children is null
                    || page.Children.Count != 0
                    || page.Entries.Count != page.EntryCount)
                    throw InventoryDataLost(
                        fence,
                        $"leaf page {page.Index} has invalid bounds");
                for (var offset = 0; offset < page.Entries.Count; offset++)
                {
                    var entry = page.Entries[offset];
                    if (entry.Index != (long)page.StartIndex + offset)
                        throw InventoryDataLost(
                            fence,
                            $"leaf page {page.Index} is reordered");
                    if (entries.Count >= root.TotalCount)
                        throw InventoryDataLost(
                            fence,
                            "leaf entries exceed the declared total");
                    entries.Add(entry);
                }
                return;
            }

            if (page.Entries is null
                || page.Entries.Count != 0
                || page.Children is null
                || page.Children.Count is < 1
                    or > MaxInventoryPageEntries)
                throw InventoryDataLost(
                    fence,
                    $"index page {page.Level}:{page.Index} has invalid bounds");
            long childStart = page.StartIndex;
            foreach (var child in page.Children)
            {
                if (child.Level != page.Level - 1
                    || child.StartIndex != childStart)
                    throw InventoryDataLost(
                        fence,
                        $"index page {page.Level}:{page.Index} is reordered");
                await ReadInventoryPageAsync(child).ConfigureAwait(false);
                childStart += child.EntryCount;
            }
            if (childStart != (long)page.StartIndex + page.EntryCount)
                throw InventoryDataLost(
                    fence,
                    $"index page {page.Level}:{page.Index} entry count changed");
        }
    }

    private static ZLinkRelocationDataLostException InventoryDataLost(
        ZLinkAggregateFence fence,
        string reason) =>
        new(
            $"Aggregate '{fence.AggregateId:N}' authoritative inventory {reason}.");

    private async ValueTask UpdateAggregateParticipantGenerationsAsync(
        ZLinkAggregateFence fence,
        IReadOnlyList<AggregateParticipantRecord> participants,
        IReadOnlyDictionary<ZLinkAuthorityKey, ulong> generations,
        CancellationToken cancellationToken)
    {
        await Parallel.ForEachAsync(
            participants,
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (participant, token) =>
            {
                var key = AggregateParticipantMetaKey(
                    fence,
                    participant.Index);
                var expectedGeneration = generations[
                    new ZLinkAuthorityKey(participant.Key)];
                for (var attempt = 0; attempt < 8; attempt++)
                {
                    var stored =
                        await ReadRecordAsync<AggregateParticipantRecord>(
                                key,
                                token)
                            .ConfigureAwait(false)
                        ?? throw new ZLinkRelocationDataLostException(
                            $"Aggregate '{fence.AggregateId:N}' participant {participant.Index} metadata is missing.");
                    if (!CryptographicOperations.FixedTimeEquals(
                            stored.Record.RequestFingerprint,
                            participant.RequestFingerprint)
                        || !string.Equals(
                            stored.Record.Key,
                            participant.Key,
                            StringComparison.Ordinal))
                        throw new ZLinkRelocationDataLostException(
                            $"Aggregate '{fence.AggregateId:N}' participant {participant.Index} metadata changed.");
                    if (stored.Record.TargetAuthorityOwnerGeneration
                        == expectedGeneration)
                        return;
                    var result = await provider.WriteAsync(
                            new ZLinkStoreWriteRequest(
                                [new ZLinkStoreCondition.Version(
                                    key,
                                    stored.Version)],
                                [new ZLinkStoreMutation.Put(
                                    key,
                                    Encode(stored.Record with
                                    {
                                        TargetAuthorityOwnerGeneration =
                                            expectedGeneration
                                    }),
                                    null)]),
                            token)
                        .ConfigureAwait(false);
                    if (result is ZLinkStoreWriteResult.Applied) return;
                }
                throw new IOException(
                    $"Aggregate '{fence.AggregateId:N}' participant {participant.Index} generation could not be stored.");
            }).ConfigureAwait(false);
    }

    private async ValueTask<bool> InstallAggregateParticipantFencesAsync(
        ZLinkAggregateFence fence,
        ZLinkAggregatePrepareRequest request,
        IReadOnlyList<StoredAuthority?> observedAuthorities,
        CancellationToken cancellationToken)
    {
        var installed = new bool[request.Participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, request.Participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                var participant = request.Participants[index];
                for (var attempt = 0; attempt < 8; attempt++)
                {
                    var authority = attempt == 0
                        ? observedAuthorities[index]
                        : await ReadAuthorityRecordAsync(
                                participant.Key,
                                knownMeta: null,
                                projectCommittedAggregate: false,
                                token)
                            .ConfigureAwait(false);
                    if (authority is null) return;
                    if (authority.Meta.AggregateFence == fence
                        && authority.Meta.AggregateParticipantIndex == index
                        && string.Equals(
                            authority.Meta.AggregateExpectedStoreVersion,
                            participant.ExpectedStoreVersion,
                            StringComparison.Ordinal))
                    {
                        installed[index] = true;
                        return;
                    }
                    if (authority.Meta.AggregateFence is not null
                        || !string.Equals(
                            authority.Version.Value,
                            participant.ExpectedStoreVersion,
                            StringComparison.Ordinal))
                        return;
                    var result = await provider.WriteAsync(
                            new ZLinkStoreWriteRequest(
                                [new ZLinkStoreCondition.Version(
                                    AuthorityMetaKey(participant.Key),
                                    authority.Version)],
                                [new ZLinkStoreMutation.Put(
                                    AuthorityMetaKey(participant.Key),
                                    Encode(authority.Meta with
                                    {
                                        AggregateFence = fence,
                                        AggregateParticipantIndex = index,
                                        AggregateExpectedStoreVersion =
                                            participant.ExpectedStoreVersion
                                    }),
                                    null)]),
                            token)
                        .ConfigureAwait(false);
                    if (result is ZLinkStoreWriteResult.Applied)
                    {
                        installed[index] = true;
                        return;
                    }
                }
            }).ConfigureAwait(false);
        return installed.All(static value => value);
    }

    private async ValueTask<bool> IsExactStagedParticipantAsync(
        ZLinkStoreKey metaKey,
        ZLinkStoreKey payloadKey,
        AggregateParticipantRecord expected,
        ReadOnlyMemory<byte> expectedPayload,
        CancellationToken cancellationToken)
    {
        var meta = await ReadRecordAsync<AggregateParticipantRecord>(
                metaKey,
                cancellationToken)
            .ConfigureAwait(false);
        var payload = await provider.ReadAsync(payloadKey, cancellationToken)
            .ConfigureAwait(false);
        return meta is not null
               && meta.Record.Index == expected.Index
               && string.Equals(
                   meta.Record.Key,
                   expected.Key,
                   StringComparison.Ordinal)
               && string.Equals(
                   meta.Record.ExpectedStoreVersion,
                   expected.ExpectedStoreVersion,
                   StringComparison.Ordinal)
               && meta.Record.OwnerTransition == expected.OwnerTransition
               && CryptographicOperations.FixedTimeEquals(
                   meta.Record.AuthorityPayloadSha256,
                   expected.AuthorityPayloadSha256)
               && CryptographicOperations.FixedTimeEquals(
                   meta.Record.MembershipMutationSha256,
                   expected.MembershipMutationSha256)
               && CryptographicOperations.FixedTimeEquals(
                   meta.Record.RequestFingerprint,
                   expected.RequestFingerprint)
               && payload is ZLinkStoreReadResult.Found found
               && CryptographicOperations.FixedTimeEquals(
                   found.Value.Bytes.Span,
                   expectedPayload.Span);
    }

    private async ValueTask<bool>
        IsExactStagedParticipantForReconciliationAsync(
            ZLinkStoreKey metaKey,
            ZLinkStoreKey payloadKey,
            AggregateParticipantRecord expected,
            ReadOnlyMemory<byte> expectedPayload)
    {
        using var deadline = new CancellationTokenSource(
            AmbiguousReconciliationTimeout);
        return await IsExactStagedParticipantAsync(
                metaKey,
                payloadKey,
                expected,
                expectedPayload,
                deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private async ValueTask<StoredAggregateParticipant[]>
        ReadAggregateParticipantsAsync(
            ZLinkAggregateFence fence,
            AggregateRecord aggregate,
        CancellationToken cancellationToken)
    {
        if (aggregate.ParticipantCount < 1)
            throw new ZLinkRelocationDataLostException(
                $"Aggregate '{fence.AggregateId:N}' has an invalid participant count.");
        var inventory = await ReadAndValidateAggregateInventoryAsync(
                fence,
                aggregate,
                cancellationToken)
            .ConfigureAwait(false);
        var participants =
            new StoredAggregateParticipant[aggregate.ParticipantCount];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, aggregate.ParticipantCount),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                var meta = await ReadRecordAsync<AggregateParticipantRecord>(
                        AggregateParticipantMetaKey(fence, index),
                        token)
                    .ConfigureAwait(false)
                    ?? throw new ZLinkRelocationDataLostException(
                        $"Aggregate '{fence.AggregateId:N}' participant {index} is missing.");
                var payload = await provider.ReadAsync(
                        AggregateParticipantPayloadKey(fence, index),
                        token)
                    .ConfigureAwait(false);
                if (payload is not ZLinkStoreReadResult.Found found
                    || meta.Record.Index != index
                    || !AggregateInventoryEntryEquals(
                        inventory[index],
                        meta.Record)
                    || !CryptographicOperations.FixedTimeEquals(
                        meta.Record.RequestFingerprint,
                        aggregate.RequestFingerprint)
                    || !CryptographicOperations.FixedTimeEquals(
                        meta.Record.AuthorityPayloadSha256,
                        Sha256(found.Value.Bytes)))
                {
                    throw new ZLinkRelocationDataLostException(
                        $"Aggregate '{fence.AggregateId:N}' participant {index} is invalid.");
                }
                participants[index] = new StoredAggregateParticipant(
                    meta.Record,
                    found.Value.Bytes);
            }).ConfigureAwait(false);
        return participants;
    }

    private async ValueTask<StoredAggregateParticipant>
        ReadAggregateParticipantAsync(
            ZLinkAggregateFence fence,
            AggregateRecord aggregate,
            int index,
            CancellationToken cancellationToken)
    {
        if (index < 0 || index >= aggregate.ParticipantCount)
            throw new ZLinkRelocationDataLostException(
                $"Aggregate '{fence.AggregateId:N}' participant index is invalid.");
        var inventoryEntry =
            await ReadAndValidateAggregateInventoryEntryAsync(
                fence,
                aggregate,
                index,
                cancellationToken)
            .ConfigureAwait(false);
        var meta = await ReadRecordAsync<AggregateParticipantRecord>(
                AggregateParticipantMetaKey(fence, index),
                cancellationToken)
            .ConfigureAwait(false)
            ?? throw new ZLinkRelocationDataLostException(
                $"Aggregate '{fence.AggregateId:N}' participant {index} is missing.");
        var payload = await provider.ReadAsync(
                AggregateParticipantPayloadKey(fence, index),
                cancellationToken)
            .ConfigureAwait(false);
        if (payload is not ZLinkStoreReadResult.Found found
            || meta.Record.Index != index
            || !AggregateInventoryEntryEquals(
                inventoryEntry,
                meta.Record)
            || !CryptographicOperations.FixedTimeEquals(
                meta.Record.RequestFingerprint,
                aggregate.RequestFingerprint)
            || !CryptographicOperations.FixedTimeEquals(
                meta.Record.AuthorityPayloadSha256,
                Sha256(found.Value.Bytes)))
            throw new ZLinkRelocationDataLostException(
                $"Aggregate '{fence.AggregateId:N}' participant {index} is invalid.");
        return new StoredAggregateParticipant(meta.Record, found.Value.Bytes);
    }

    private async ValueTask<AggregateInventoryLeafEntry>
        ReadAndValidateAggregateInventoryEntryAsync(
            ZLinkAggregateFence fence,
            AggregateRecord aggregate,
            int participantIndex,
            CancellationToken cancellationToken)
    {
        var root = aggregate.Inventory;
        if (root is null
            || root.TotalCount != aggregate.ParticipantCount
            || participantIndex < 0
            || participantIndex >= root.TotalCount
            || root.TopLevel < 0
            || root.TopLevel >= MaxInventoryTreeLevels
            || root.TopPages is null
            || root.TopPages.Count is < 1 or > MaxInventoryPageEntries
            || root.PageCountsByLevel is null
            || root.PageCountsByLevel.Count != root.TopLevel + 1
            || root.Digest is null
            || root.Digest.Length != SHA256.HashSizeInBytes)
            throw InventoryDataLost(
                fence,
                "root metadata is invalid");

        AggregateInventoryPageReference? selected = null;
        long expectedStart = 0;
        foreach (var reference in root.TopPages)
        {
            if (reference.Level != root.TopLevel
                || reference.StartIndex != expectedStart
                || reference.EntryCount < 1
                || reference.Sha256 is null
                || reference.Sha256.Length != SHA256.HashSizeInBytes)
                throw InventoryDataLost(
                    fence,
                    "top page references are invalid or reordered");
            var end = checked(
                (long)reference.StartIndex + reference.EntryCount);
            if (participantIndex >= reference.StartIndex
                && participantIndex < end)
                selected = reference;
            expectedStart = end;
        }
        if (expectedStart != root.TotalCount || selected is null)
            throw InventoryDataLost(
                fence,
                "top page references do not contain the participant");
        return await ReadEntryAsync(selected).ConfigureAwait(false);

        async ValueTask<AggregateInventoryLeafEntry> ReadEntryAsync(
            AggregateInventoryPageReference reference)
        {
            var read = await provider.ReadAsync(
                    AggregateInventoryPageKey(
                        fence,
                        reference.Level,
                        reference.Index),
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkStoreReadResult.Found found
                || found.Value.Bytes.Length > MaxInventoryPageBytes
                || !CryptographicOperations.FixedTimeEquals(
                    SHA256.HashData(found.Value.Bytes.Span),
                    reference.Sha256))
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} is missing or corrupt");
            AggregateInventoryPage page;
            try
            {
                page = DecodeAggregateInventoryPage(found.Value.Bytes);
            }
            catch (Exception error) when (
                error is InvalidDataException
                    or DecoderFallbackException
                    or JsonException
                    or NotSupportedException
                    or ArgumentException)
            {
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} cannot be decoded ({error.GetType().Name})");
            }
            if (page.Level != reference.Level
                || page.Index != reference.Index
                || page.StartIndex != reference.StartIndex
                || page.EntryCount != reference.EntryCount)
                throw InventoryDataLost(
                    fence,
                    $"page {reference.Level}:{reference.Index} metadata changed");
            if (page.Level == 0)
            {
                if (page.Entries is null
                    || page.Entries.Count != page.EntryCount
                    || page.Children is null
                    || page.Children.Count != 0)
                    throw InventoryDataLost(
                        fence,
                        $"leaf page {page.Index} has invalid bounds");
                var offset = participantIndex - page.StartIndex;
                if (offset < 0 || offset >= page.Entries.Count)
                    throw InventoryDataLost(
                        fence,
                        $"leaf page {page.Index} does not contain the participant");
                var entry = page.Entries[offset];
                if (entry.Index != participantIndex)
                    throw InventoryDataLost(
                        fence,
                        $"leaf page {page.Index} participant is reordered");
                return entry;
            }

            if (page.Entries is null
                || page.Entries.Count != 0
                || page.Children is null
                || page.Children.Count is < 1 or > MaxInventoryPageEntries)
                throw InventoryDataLost(
                    fence,
                    $"index page {page.Level}:{page.Index} has invalid bounds");
            AggregateInventoryPageReference? child = null;
            long childStart = page.StartIndex;
            foreach (var candidate in page.Children)
            {
                if (candidate.Level != page.Level - 1
                    || candidate.StartIndex != childStart
                    || candidate.EntryCount < 1)
                    throw InventoryDataLost(
                        fence,
                        $"index page {page.Level}:{page.Index} is reordered");
                var childEnd = checked(
                    (long)candidate.StartIndex + candidate.EntryCount);
                if (participantIndex >= candidate.StartIndex
                    && participantIndex < childEnd)
                    child = candidate;
                childStart = childEnd;
            }
            if (childStart != (long)page.StartIndex + page.EntryCount
                || child is null)
                throw InventoryDataLost(
                    fence,
                    $"index page {page.Level}:{page.Index} does not contain the participant");
            return await ReadEntryAsync(child).ConfigureAwait(false);
        }
    }

    private static bool AggregateInventoryEntryEquals(
        AggregateInventoryLeafEntry entry,
        AggregateParticipantRecord participant) =>
        entry.Index == participant.Index
        && string.Equals(entry.Key, participant.Key, StringComparison.Ordinal)
        && string.Equals(
            entry.ExpectedStoreVersion,
            participant.ExpectedStoreVersion,
            StringComparison.Ordinal)
        && entry.OwnerTransition == participant.OwnerTransition
        && CryptographicOperations.FixedTimeEquals(
            entry.AuthorityPayloadSha256,
            participant.AuthorityPayloadSha256)
        && CryptographicOperations.FixedTimeEquals(
            entry.MembershipMutationSha256,
            participant.MembershipMutationSha256);

    private async ValueTask<IReadOnlyDictionary<ZLinkAuthorityKey, ulong>>
        ReadCurrentAuthorityGenerationsAsync(
            IReadOnlyList<ZLinkAggregateParticipant> participants,
            CancellationToken cancellationToken)
    {
        var generations = new ulong[participants.Count];
        await Parallel.ForEachAsync(
            Enumerable.Range(0, participants.Count),
            new ParallelOptions
            {
                MaxDegreeOfParallelism = 64,
                CancellationToken = cancellationToken
            },
            async (index, token) =>
            {
                var authority = await ReadAuthorityRecordAsync(
                        participants[index].Key,
                        token)
                    .ConfigureAwait(false)
                    ?? throw new InvalidDataException(
                        $"Aggregate participant authority '{participants[index].Key.Value}' is missing.");
                if (authority.Meta.AuthorityOwnerGeneration == 0)
                    throw new InvalidDataException(
                        $"Aggregate participant authority '{participants[index].Key.Value}' has an invalid generation.");
                generations[index] =
                    authority.Meta.AuthorityOwnerGeneration;
            }).ConfigureAwait(false);
        return participants
            .Select((participant, index) => new
            {
                participant.Key,
                Generation = generations[index]
            })
            .ToDictionary(
                static pair => pair.Key,
                static pair => pair.Generation);
    }

    private async ValueTask<ZLinkAggregatePrepareResult>
        ReconcileExistingAggregateAsync(
            ZLinkAggregateFence fence,
            AggregateRecord aggregate,
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken)
    {
        if (aggregate.Status is not (
                AggregateStatus.Prepared or AggregateStatus.Committed)
            || !AggregateHeaderEquals(
                aggregate.Header,
                CreateAggregateHeader(request))
            || !CryptographicOperations.FixedTimeEquals(
                aggregate.RequestFingerprint,
                AggregateRequestFingerprint(request))
            || aggregate.ParticipantCount != request.Participants.Count
            || !AggregateInventoryRootEquals(
                aggregate.Inventory,
                BuildAggregateInventory(request).Root))
            return new ZLinkAggregatePrepareResult.Conflict();

        IReadOnlyDictionary<ZLinkAuthorityKey, ulong> generations;
        if (!aggregate.ParticipantsNormalized)
        {
            var participants = await ReadAggregateParticipantsAsync(
                    fence,
                    aggregate,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!AggregateParticipantsEqual(
                    participants,
                    request.Participants))
                return new ZLinkAggregatePrepareResult.Conflict();
            generations = participants.ToDictionary(
                static participant =>
                    new ZLinkAuthorityKey(participant.Metadata.Key),
                static participant =>
                    participant.Metadata.TargetAuthorityOwnerGeneration);
        }
        else
            generations = await ReadCurrentAuthorityGenerationsAsync(
                    request.Participants,
                    cancellationToken)
                .ConfigureAwait(false);
        return new ZLinkAggregatePrepareResult.AlreadyPrepared(fence)
        {
            TargetAuthorityOwnerGenerations = generations
        };
    }

    private async ValueTask<ZLinkAggregatePrepareResult>
        ReconcileExistingAggregateForAmbiguousAsync(
            ZLinkAggregateFence fence,
            AggregateRecord aggregate,
            ZLinkAggregatePrepareRequest request)
    {
        using var deadline = new CancellationTokenSource(
            AmbiguousReconciliationTimeout);
        return await ReconcileExistingAggregateAsync(
                fence,
                aggregate,
                request,
                deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private async ValueTask NormalizeCommittedAggregateForAmbiguousAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate)
    {
        using var deadline = new CancellationTokenSource(
            AmbiguousReconciliationTimeout);
        await NormalizeCommittedAggregateAsync(
                fence,
                aggregate,
                deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private static bool AggregateParticipantsEqual(
        IReadOnlyList<StoredAggregateParticipant> stored,
        IReadOnlyList<ZLinkAggregateParticipant> requested)
    {
        if (stored.Count != requested.Count) return false;
        for (var index = 0; index < stored.Count; index++)
        {
            var actual = stored[index];
            var expected = requested[index];
            if (!string.Equals(
                    actual.Metadata.Key,
                    expected.Key.Value,
                    StringComparison.Ordinal)
                || !string.Equals(
                    actual.Metadata.ExpectedStoreVersion,
                    expected.ExpectedStoreVersion,
                    StringComparison.Ordinal)
                || actual.Metadata.OwnerTransition
                != expected.OwnerTransition
                || !CryptographicOperations.FixedTimeEquals(
                    actual.Payload.Span,
                    expected.AuthorityPayload.Span)
                || !CryptographicOperations.FixedTimeEquals(
                    actual.Metadata.MembershipMutationSha256,
                    Sha256(expected.MembershipMutation)))
                return false;
        }
        return true;
    }

    private static ZLinkAggregatePrepareRequest RehydrateAggregateRequest(
        AggregateRecord aggregate,
        IReadOnlyList<StoredAggregateParticipant> participants) =>
        new(
            aggregate.Header.AggregateId,
            aggregate.Header.AggregateGeneration,
            participants.Select(static participant =>
                    new ZLinkAggregateParticipant(
                        new ZLinkAuthorityKey(participant.Metadata.Key),
                        participant.Metadata.ExpectedStoreVersion,
                        participant.Metadata.OwnerTransition,
                        participant.Payload,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            aggregate.Header.InventoryDigest,
            aggregate.Header.TargetDescriptor,
            aggregate.Header.TargetDescriptorLifecycleGeneration,
            aggregate.Header.Capacity,
            aggregate.Header.TargetOwner);

    private static AggregateHeader CreateAggregateHeader(
        ZLinkAggregatePrepareRequest request) =>
        new(
            request.AggregateId,
            request.AggregateGeneration,
            request.InventoryDigest.ToArray(),
            request.TargetDescriptor,
            request.TargetDescriptorLifecycleGeneration,
            request.Capacity,
            request.TargetOwner);

    private static bool AggregateHeaderEquals(
        AggregateHeader left,
        AggregateHeader right) =>
        EncodeAggregateHeader(left)
            .AsSpan()
            .SequenceEqual(EncodeAggregateHeader(right));

    private static byte[] AggregateRequestFingerprint(
        ZLinkAggregatePrepareRequest request)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData(EncodeAggregateHeader(
            CreateAggregateHeader(request)));
        using var stream = new MemoryStream();
        WriteAggregateFingerprintHeader(
            stream,
            AggregateRequestRecordKind);
        WriteInventoryInt32(stream, request.Participants.Count);
        foreach (var participant in request.Participants.OrderBy(
                     static participant => participant.Key.Value,
                     StringComparer.Ordinal))
        {
            WriteInventoryString(stream, participant.Key.Value);
            WriteInventoryString(
                stream,
                participant.ExpectedStoreVersion);
            stream.WriteByte((byte)participant.OwnerTransition);
            WriteInventoryHash(
                stream,
                Sha256(participant.AuthorityPayload));
            WriteInventoryHash(
                stream,
                Sha256(participant.MembershipMutation));
        }
        hash.AppendData(stream.GetBuffer().AsSpan(
            0,
            checked((int)stream.Length)));
        return hash.GetHashAndReset();
    }

    private static byte[] EncodeAggregateHeader(
        AggregateHeader header)
    {
        using var stream = new MemoryStream();
        WriteAggregateFingerprintHeader(
            stream,
            AggregateHeaderRecordKind);
        stream.Write(header.AggregateId.ToByteArray());
        WriteInventoryUInt64(stream, header.AggregateGeneration);
        WriteInventoryHash(stream, header.InventoryDigest);
        WriteInventoryString(
            stream,
            header.TargetDescriptor.MeshName);
        WriteInventoryString(
            stream,
            header.TargetDescriptor.Rid.ToHex());
        WriteInventoryUInt64(
            stream,
            header.TargetDescriptorLifecycleGeneration);
        WriteInventoryInt32(stream, header.Capacity.Actors);
        WriteInventoryInt32(stream, header.Capacity.Spots);
        if (header.Capacity.SpotType is { } spotType)
        {
            stream.WriteByte(1);
            stream.WriteByte((byte)spotType.ObjectKind);
            WriteInventoryString(stream, spotType.StableType);
            WriteInventoryInt32(stream, spotType.Count);
        }
        else
            stream.WriteByte(0);
        WriteInventoryString(stream, header.TargetOwner.OwnerId);
        WriteInventoryInt64(
            stream,
            header.TargetOwner.LeaseGeneration);
        return stream.ToArray();
    }

    private static void WriteAggregateFingerprintHeader(
        Stream stream,
        byte recordKind)
    {
        Span<byte> header = stackalloc byte[6];
        BinaryPrimitives.WriteUInt32BigEndian(
            header,
            AggregateFingerprintMagic);
        header[4] = AggregateFingerprintVersion;
        header[5] = recordKind;
        stream.Write(header);
    }

    private static void WriteInventoryUInt64(
        Stream stream,
        ulong value)
    {
        Span<byte> encoded = stackalloc byte[sizeof(ulong)];
        BinaryPrimitives.WriteUInt64BigEndian(encoded, value);
        stream.Write(encoded);
    }

    private static void WriteInventoryInt64(
        Stream stream,
        long value)
    {
        Span<byte> encoded = stackalloc byte[sizeof(long)];
        BinaryPrimitives.WriteInt64BigEndian(encoded, value);
        stream.Write(encoded);
    }

    private static ZLinkAggregatePrepareRequest
        CanonicalizeAggregateRequest(
            ZLinkAggregatePrepareRequest request)
    {
        var participants = request.Participants
            .OrderBy(
                static participant => participant.Key.Value,
                StringComparer.Ordinal)
            .ToArray();
        return request with { Participants = participants };
    }

    private async ValueTask<ZLinkAggregatePrepareResult>
        ReconcileAggregatePrepareAsync(
            ZLinkStoreKey key,
            ZLinkAggregateFence fence,
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken)
    {
        var stored = await ReadRecordAsync<AggregateRecord>(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        return stored is null
            ? new ZLinkAggregatePrepareResult.Conflict()
            : await ReconcileExistingAggregateAsync(
                    fence,
                    stored.Record,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkAggregateAbortResult>
        AbortAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
    {
        var key = AggregateKey(fence);
        var aggregate = await ReadRecordAsync<AggregateRecord>(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        if (aggregate is null) return ZLinkAggregateAbortResult.Stale;
        if (aggregate.Record.Status == AggregateStatus.Aborted)
        {
            await ReleaseAggregateParticipantFencesAsync(
                    fence,
                    aggregate.Record,
                    cancellationToken)
                .ConfigureAwait(false);
            await CleanupAggregateParticipantsAsync(
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
            return ZLinkAggregateAbortResult.AlreadyAborted;
        }
        if (aggregate.Record.Status == AggregateStatus.Staging)
        {
            var stagedAbort = await provider.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Version(key, aggregate.Version)],
                        [new ZLinkStoreMutation.Put(
                            key,
                            Encode(aggregate.Record with
                            {
                                Status = AggregateStatus.Aborted
                            }),
                            null)]),
                    cancellationToken)
                .ConfigureAwait(false);
            if (stagedAbort is not ZLinkStoreWriteResult.Applied)
                return ZLinkAggregateAbortResult.Stale;
            await ReleaseAggregateParticipantFencesAsync(
                    fence,
                    aggregate.Record,
                    cancellationToken)
                .ConfigureAwait(false);
            await CleanupAggregateParticipantsAsync(
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
            return ZLinkAggregateAbortResult.Aborted;
        }
        if (aggregate.Record.Status == AggregateStatus.Committed)
            return ZLinkAggregateAbortResult.Stale;
        // Abort needs the target capacity vector from the durable aggregate
        // header. Participant meta is not authoritative for fence release and
        // may already be missing after a partial cleanup.
        var header = aggregate.Record.Header;
        var capacity = await ReadCapacityAsync(
                header.TargetDescriptor,
                header.TargetDescriptorLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacityVector(
            nextCapacity,
            header.Capacity,
            pendingDelta: -1);
        var conditions = new List<ZLinkStoreCondition>
        {
            new ZLinkStoreCondition.Version(key, aggregate.Version),
            capacity.Condition
        };
        var mutations = new List<ZLinkStoreMutation>
        {
            new ZLinkStoreMutation.Put(
                key,
                Encode(aggregate.Record with { Status = AggregateStatus.Aborted }),
                null),
            new ZLinkStoreMutation.Put(
                capacity.Key,
                Encode(nextCapacity),
                null)
        };
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(conditions, mutations),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkStoreWriteResult.Applied)
            return ZLinkAggregateAbortResult.Stale;
        await ReleaseAggregateParticipantFencesAsync(
                fence,
                aggregate.Record,
                cancellationToken)
            .ConfigureAwait(false);
        await CleanupAggregateParticipantsAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
        return ZLinkAggregateAbortResult.Aborted;
    }

    private async ValueTask<ZLinkObjectCommitResult> CompleteCommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        int counterRetry,
        DateTimeOffset retryDeadline,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(reservation);
        ValidateAuthorityPayload(readyPayload);
        var current = await ReadAuthorityRecordAsync(
                reservation.Key,
                cancellationToken)
            .ConfigureAwait(false);
        var storedReservation = await ReadRecordAsync<ReservationRecord>(
                ReservationKey(reservation.ReservationVersion),
                cancellationToken)
            .ConfigureAwait(false);
        if (storedReservation?.Record.Status == ReservationStatus.Created)
            return current is null
                ? new ZLinkObjectCommitResult.Stale()
                : new ZLinkObjectCommitResult.AlreadyCommitted(current.Snapshot);
        if (!MatchesReservation(current, storedReservation, reservation))
            return new ZLinkObjectCommitResult.Stale();
        var target = await ReadEligibleTargetAsync(
                reservation.TargetDescriptor,
                reservation.TargetNodeLifecycleGeneration,
                reservation.TargetOwner,
                current!.Snapshot.Allocation.ObjectKind,
                current.Snapshot.Allocation.StableType,
                cancellationToken,
                requireNewPlacementEligibility: false)
            .ConfigureAwait(false);
        if (target is null) return new ZLinkObjectCommitResult.Stale();
        var capacity = await ReadCapacityAsync(
                reservation.TargetDescriptor,
                reservation.TargetNodeLifecycleGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        var nextCapacity = capacity.Record.Clone();
        ApplyCapacity(
            nextCapacity,
            current.Snapshot.Allocation,
            pendingDelta: -1,
            activeDelta: 1);
        var meta = current.Meta with
        {
            PayloadSha256 = Sha256(readyPayload),
            Allocation = current.Snapshot.Allocation with
            {
                State = ZLinkPlacementAllocationState.Active
            },
            ReservedCreation = null
        };
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        AuthorityMetaKey(reservation.Key),
                        current.Version),
                    new ZLinkStoreCondition.Version(
                        ReservationKey(reservation.ReservationVersion),
                        storedReservation!.Version),
                    target.DescriptorCondition,
                    target.OwnerCondition,
                    capacity.Condition
                ],
                [
                    new ZLinkStoreMutation.Put(
                        AuthorityMetaKey(reservation.Key),
                        Encode(meta),
                        null),
                    new ZLinkStoreMutation.Put(
                        AuthorityPayloadKey(reservation.Key),
                        readyPayload.ToArray(),
                        null),
                    new ZLinkStoreMutation.Put(
                        ReservationKey(reservation.ReservationVersion),
                        Encode(storedReservation.Record with
                        {
                            Status = ReservationStatus.Created
                        }),
                        null),
                    new ZLinkStoreMutation.Put(
                        capacity.Key,
                        Encode(nextCapacity),
                        null)
                ]),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkStoreWriteResult.Applied applied)
        {
            if (counterRetry < CounterRetryLimit
                && DateTimeOffset.UtcNow < retryDeadline)
            {
                var unchangedAuthority = await ReadAuthorityRecordAsync(
                        reservation.Key,
                        cancellationToken)
                    .ConfigureAwait(false);
                var unchangedReservation =
                    await ReadRecordAsync<ReservationRecord>(
                            ReservationKey(reservation.ReservationVersion),
                            cancellationToken)
                        .ConfigureAwait(false);
                if (unchangedReservation?.Record.Status
                    == ReservationStatus.Created)
                    return unchangedAuthority is null
                        ? new ZLinkObjectCommitResult.Stale()
                        : new ZLinkObjectCommitResult.AlreadyCommitted(
                            unchangedAuthority.Snapshot);
                if (MatchesReservation(
                        unchangedAuthority,
                        unchangedReservation,
                        reservation))
                {
                    await DelayCounterRetryAsync(
                            counterRetry,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                    return await CompleteCommitAsync(
                            reservation,
                            readyPayload,
                            counterRetry + 1,
                            retryDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            return new ZLinkObjectCommitResult.Stale();
        }
        return new ZLinkObjectCommitResult.Committed(
            Snapshot(
                meta,
                applied.PutVersions[AuthorityMetaKey(reservation.Key)],
                applied.StoreNow,
                readyPayload));
    }

    private async ValueTask<ZLinkAuthorityCompareExchangeResult> StoreAuthorityAsync(
        StoredAuthority current,
        AuthorityMeta meta,
        ReadOnlyMemory<byte> payload,
        IReadOnlyList<ZLinkStoreCondition> conditions,
        IReadOnlyList<ZLinkStoreMutation> extraMutations,
        CancellationToken cancellationToken)
    {
        var metaKey = AuthorityMetaKey(current.Key);
        var mutations = new List<ZLinkStoreMutation>(extraMutations)
        {
            new ZLinkStoreMutation.Put(metaKey, Encode(meta), null),
            new ZLinkStoreMutation.Put(
                AuthorityPayloadKey(current.Key),
                payload.ToArray(),
                null)
        };
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(conditions, mutations),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkStoreWriteResult.Applied applied)
            return Conflict(await ReadAuthorityRecordAsync(
                    current.Key,
                    cancellationToken)
                .ConfigureAwait(false));
        return new ZLinkAuthorityCompareExchangeResult.Stored(
            Snapshot(
                meta,
                applied.PutVersions[metaKey],
                applied.StoreNow,
                payload));
    }

    private async ValueTask<StoredAuthority?> ReadAuthorityRecordAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken) =>
        await ReadAuthorityRecordAsync(
                key,
                null,
                projectCommittedAggregate: true,
                cancellationToken)
            .ConfigureAwait(false);

    private async ValueTask<StoredAuthority?> ReadAuthorityRecordAsync(
        ZLinkAuthorityKey key,
        ZLinkStoreValue? knownMeta,
        bool projectCommittedAggregate,
        CancellationToken cancellationToken)
    {
        var metaKey = AuthorityMetaKey(key);
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var first = knownMeta is null
                ? await provider.ReadAsync(metaKey, cancellationToken)
                    .ConfigureAwait(false)
                : new ZLinkStoreReadResult.Found(knownMeta);
            knownMeta = null;
            if (first is ZLinkStoreReadResult.Missing) return null;
            var found = ((ZLinkStoreReadResult.Found)first).Value;
            var meta = Decode<AuthorityMeta>(found.Bytes);
            ReadOnlyMemory<byte> payloadBytes;
            if (projectCommittedAggregate
                && meta.AggregateFence is { } aggregateFence)
            {
                var aggregate = await ReadRecordAsync<AggregateRecord>(
                        AggregateKey(aggregateFence),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (aggregate?.Record.Status == AggregateStatus.Committed
                    && !aggregate.Record.ParticipantsNormalized)
                {
                    if (meta.AggregateParticipantIndex is not { } index)
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate '{aggregateFence.AggregateId:N}' "
                            + $"has no participant index for authority '{key.Value}'.");
                    StoredAggregateParticipant participant;
                    try
                    {
                        participant = await ReadAggregateParticipantAsync(
                                aggregateFence,
                                aggregate.Record,
                                index,
                                cancellationToken)
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
                        var currentMeta = await provider.ReadAsync(
                                metaKey,
                                cancellationToken)
                            .ConfigureAwait(false);
                        if (currentMeta is not ZLinkStoreReadResult.Found current
                            || current.Value.Version != found.Version)
                            continue;
                        throw;
                    }
                    if (!string.Equals(
                            participant.Metadata.Key,
                            key.Value,
                            StringComparison.Ordinal))
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate '{aggregateFence.AggregateId:N}' "
                            + $"does not contain authority '{key.Value}'.");
                    var targetAuthorityOwnerGeneration =
                        participant.Metadata
                            .TargetAuthorityOwnerGeneration;
                    if (targetAuthorityOwnerGeneration == 0)
                        throw new ZLinkRelocationDataLostException(
                            $"Committed aggregate '{aggregateFence.AggregateId:N}' "
                            + $"has no target generation for authority '{key.Value}'.");
                    var changesOwner = participant.Metadata.OwnerTransition
                                       == ZLinkAuthorityGenerationTransition.NewOwner;
                    meta = meta with
                    {
                        PayloadSha256 =
                            participant.Metadata.AuthorityPayloadSha256,
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
                                Descriptor =
                                    aggregate.Record.Header.TargetDescriptor,
                                DescriptorLifecycleGeneration =
                                    aggregate.Record.Header
                                        .TargetDescriptorLifecycleGeneration
                            }
                            : meta.Allocation
                    };
                    payloadBytes = participant.Payload;
                }
                else
                {
                    payloadBytes = await ReadAuthorityPayloadAsync(
                            key,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            else
            {
                payloadBytes = await ReadAuthorityPayloadAsync(
                        key,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            var verify = await provider.ReadAsync(metaKey, cancellationToken)
                .ConfigureAwait(false);
            if (verify is not ZLinkStoreReadResult.Found verified
                || verified.Value.Version != found.Version)
                continue;
            if (!CryptographicOperations.FixedTimeEquals(
                    meta.PayloadSha256,
                    Sha256(payloadBytes)))
                throw new InvalidDataException(
                    $"Authority '{key.Value}' payload checksum is invalid.");
            return new StoredAuthority(
                key,
                meta,
                found.Version,
                Snapshot(
                    meta,
                    found.Version,
                    verified.Value.StoreNow,
                    payloadBytes));
        }
        throw new IOException(
            $"Authority '{key.Value}' changed continuously while it was read.");
    }

    private async ValueTask<ReadOnlyMemory<byte>> ReadAuthorityPayloadAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken)
    {
        var payload = await provider.ReadAsync(
                AuthorityPayloadKey(key),
                cancellationToken)
            .ConfigureAwait(false);
        return payload is ZLinkStoreReadResult.Found found
            ? found.Value.Bytes
            : throw new InvalidDataException(
                $"Authority '{key.Value}' has no payload.");
    }

    private async ValueTask<StoredTarget?> ReadEligibleTargetAsync(
        ZLinkMeshNodeDescriptorKey key,
        ulong lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind? objectKind,
        string? stableType,
        CancellationToken cancellationToken,
        bool requireNewPlacementEligibility = true,
        bool allowPreparingTarget = false)
    {
        var descriptorKey = MeshKey(key.MeshName, key.Rid);
        var descriptorRead = await provider.ReadAsync(
                descriptorKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (descriptorRead is not ZLinkStoreReadResult.Found descriptorFound)
        {
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"eligible_target_rejected reason=no_descriptor mesh={key.MeshName}");
            return null;
        }
        var descriptor = Decode<MeshRecord>(descriptorFound.Value.Bytes).Descriptor;
        var ownerRead = await ReadLiveOwnerAsync(owner, cancellationToken)
            .ConfigureAwait(false);
        //  Nine guards below share one bare null. Name the values they compare
        //  so a rejection says which one disagreed.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"eligible_target_check rid={key.Rid} owner_live={ownerRead is not null} "
            + $"lifecycle={descriptor.LifecycleGeneration}/{lifecycleGeneration} "
            + $"owner={descriptor.OwnerId}/{owner.OwnerId} "
            + $"lease={descriptor.LeaseGeneration}/{owner.LeaseGeneration} "
            + $"role={descriptor.ObjectRole} state={descriptor.State} "
            + $"weight={descriptor.PlacementWeight} kind={objectKind} "
            + $"stable_type={stableType}");
        if (ownerRead is null
            || descriptor.MeshName != key.MeshName
            || descriptor.Rid != key.Rid
            || descriptor.LifecycleGeneration != lifecycleGeneration
            || descriptor.OwnerId != owner.OwnerId
            || descriptor.LeaseGeneration != owner.LeaseGeneration
            || descriptor.ObjectRole != ZLinkMeshNodeObjectRole.Server
            || (requireNewPlacementEligibility
                && (descriptor.State != ZLinkFrameworkRuntimeState.Serving
                    && !(allowPreparingTarget
                         && descriptor.State
                         == ZLinkFrameworkRuntimeState.Preparing)
                    || descriptor.PlacementWeight <= 0)))
            return null;
        if (objectKind is { } kind
            && !descriptor.ObjectCapabilities.Any(capability =>
                capability.ObjectKind == kind
                && string.Equals(
                    capability.StableType,
                    stableType,
                    StringComparison.Ordinal)))
            return null;
        return new StoredTarget(
            descriptor,
            new ZLinkStoreCondition.Version(
                descriptorKey,
                descriptorFound.Value.Version),
            new ZLinkStoreCondition.Version(
                OwnerKey(owner.OwnerId),
                ownerRead.Version));
    }

    //  Used only when committing a relocation whose target was already chosen
    //  and reserved. Selection is long past, so the new-placement rules that
    //  govern selection must not be re-applied here; a target weighted out of
    //  the candidate pool after admission would otherwise fail the commit of a
    //  relocation it had already accepted.
    private async ValueTask<bool> IsEligibleTargetAsync(
        ZLinkMeshNodeDescriptorKey key,
        ulong lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind objectKind,
        string stableType,
        CancellationToken cancellationToken) =>
        await ReadEligibleTargetAsync(
                key,
                lifecycleGeneration,
                owner,
                objectKind,
                stableType,
                cancellationToken,
                requireNewPlacementEligibility: false)
            .ConfigureAwait(false) is not null;

    private async ValueTask<StoredOwner?> ReadLiveOwnerAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken)
    {
        var read = await provider.ReadAsync(
                OwnerKey(token.OwnerId),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkStoreReadResult.Found found) return null;
        var record = Decode<OwnerRecord>(found.Value.Bytes);
        return record.OwnerId == token.OwnerId
               && record.LeaseGeneration == token.LeaseGeneration
            ? new StoredOwner(token, found.Value.Version)
            : null;
    }

    private async ValueTask<StoredCapacity> ReadCapacityAsync(
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong lifecycleGeneration,
        CancellationToken cancellationToken)
    {
        var key = CapacityKey(descriptor, lifecycleGeneration);
        var read = await provider.ReadAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return read switch
        {
            ZLinkStoreReadResult.Missing => new StoredCapacity(
                key,
                new CapacityRecord(),
                new ZLinkStoreCondition.Missing(key)),
            ZLinkStoreReadResult.Found found => new StoredCapacity(
                key,
                Decode<CapacityRecord>(found.Value.Bytes),
                new ZLinkStoreCondition.Version(key, found.Value.Version)),
            _ => throw new InvalidOperationException()
        };
    }

    private async ValueTask<GenerationState> ReadGenerationAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken)
    {
        var storeKey = GenerationKey(key);
        var read = await provider.ReadAsync(storeKey, cancellationToken)
            .ConfigureAwait(false);
        return read switch
        {
            ZLinkStoreReadResult.Missing => new GenerationState(
                0,
                0,
                new ZLinkStoreCondition.Missing(storeKey)),
            ZLinkStoreReadResult.Found found => new GenerationState(
                Decode<GenerationRecord>(found.Value.Bytes).ObjectGeneration,
                Decode<GenerationRecord>(found.Value.Bytes)
                    .AuthorityOwnerGeneration,
                new ZLinkStoreCondition.Version(
                    storeKey,
                    found.Value.Version)),
            _ => throw new InvalidOperationException()
        };
    }

    private async ValueTask<StoredTerminal?> ReadTerminalAsync(
        ZLinkCreationOperationId operation,
        CancellationToken cancellationToken)
    {
        var key = TerminalKey(operation);
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var metaRead = await provider.ReadAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (metaRead is not ZLinkStoreReadResult.Found metaFound)
                return null;
            var meta = Decode<TerminalMeta>(metaFound.Value.Bytes);
            var payloadRead = await provider.ReadAsync(
                    TerminalPayloadKey(operation),
                    cancellationToken)
                .ConfigureAwait(false);
            if (payloadRead is not ZLinkStoreReadResult.Found payloadFound)
                return null;
            var verify = await provider.ReadAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (verify is not ZLinkStoreReadResult.Found verified
                || verified.Value.Version != metaFound.Value.Version)
                continue;
            if (!CryptographicOperations.FixedTimeEquals(
                    meta.PayloadSha256,
                    Sha256(payloadFound.Value.Bytes)))
                throw new InvalidDataException(
                    "The creation terminal payload checksum is invalid.");
            return new StoredTerminal(
                meta.Record with
                {
                    TerminalEnvelope = payloadFound.Value.Bytes,
                    StoreNow = verified.Value.StoreNow
                },
                metaFound.Value.Version);
        }
        throw new IOException(
            "The creation terminal changed continuously while it was read.");
    }

    private static async ValueTask DelayCounterRetryAsync(
        int retry,
        DateTimeOffset deadline,
        CancellationToken cancellationToken)
    {
        var remaining = deadline - DateTimeOffset.UtcNow;
        if (remaining <= TimeSpan.Zero)
            return;
        var exponentialMilliseconds = Math.Min(
            100,
            2 << Math.Min(retry, 5));
        var delay = TimeSpan.FromMilliseconds(
            Math.Min(
                remaining.TotalMilliseconds,
                exponentialMilliseconds
                + Random.Shared.Next(0, exponentialMilliseconds + 1)));
        if (delay > TimeSpan.Zero)
            await Task.Delay(delay, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<StoredRecord<T>?> ReadRecordAsync<T>(
        ZLinkStoreKey key,
        CancellationToken cancellationToken)
    {
        var read = await provider.ReadAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkStoreReadResult.Found found)
            return null;
        try
        {
            return new StoredRecord<T>(
                Decode<T>(found.Value.Bytes),
                found.Value.Version,
                found.Value.StoreNow);
        }
        catch (Exception error) when (
            IsAggregateDurableRecord<T>()
            && error is JsonException
                or InvalidDataException
                or NotSupportedException
                or ArgumentException)
        {
            throw new ZLinkRelocationDataLostException(
                $"Aggregate durable record '{key.Value}' cannot be decoded ({error.GetType().Name}).");
        }
    }

    private static bool IsAggregateDurableRecord<T>() =>
        typeof(T) == typeof(AggregateRecord)
        || typeof(T) == typeof(AggregateParticipantRecord);

    private async ValueTask<StoredRecord<T>?>
        ReadRecordForReconciliationAsync<T>(ZLinkStoreKey key)
    {
        using var deadline = new CancellationTokenSource(
            AmbiguousReconciliationTimeout);
        return await ReadRecordAsync<T>(key, deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkAuthorityReadResult>
        ReadAuthorityForReconciliationAsync(ZLinkAuthorityKey key)
    {
        using var deadline = new CancellationTokenSource(
            AmbiguousReconciliationTimeout);
        return await ReadAuthorityAsync(key, deadline.Token)
            .AsTask()
            .WaitAsync(deadline.Token)
            .ConfigureAwait(false);
    }

    private async ValueTask<AuthorityOwnerGenerationCounterState>
        ReadAuthorityOwnerGenerationCounterAsync(
            CancellationToken cancellationToken)
    {
        var key = AuthorityOwnerGenerationCounterKey();
        var read = await provider.ReadAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return read switch
        {
            ZLinkStoreReadResult.Missing =>
                new AuthorityOwnerGenerationCounterState(
                    0,
                    new ZLinkStoreCondition.Missing(key)),
            ZLinkStoreReadResult.Found found =>
                new AuthorityOwnerGenerationCounterState(
                    Decode<AuthorityOwnerGenerationCounter>(
                        found.Value.Bytes).Value,
                    new ZLinkStoreCondition.Version(
                        key,
                        found.Value.Version)),
            _ => throw new InvalidOperationException(
                "The provider returned an invalid authority generation counter result.")
        };
    }

    private async ValueTask<DateTimeOffset> ReadStoreNowAsync(
        CancellationToken cancellationToken)
    {
        var read = await provider.ReadAsync(
                Key(Prefix + "clock"),
                cancellationToken)
            .ConfigureAwait(false);
        return read switch
        {
            ZLinkStoreReadResult.Missing missing => missing.StoreNow,
            ZLinkStoreReadResult.Found found => found.Value.StoreNow,
            _ => throw new InvalidOperationException()
        };
    }

    private async ValueTask<ZLinkObjectReserveResult> ReserveConflictAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken)
    {
        var current = await ReadAuthorityRecordAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return current is null
            ? new ZLinkObjectReserveResult.Conflict(
                new ZLinkAuthorityReadResult.Missing(
                    await ReadStoreNowAsync(cancellationToken)
                        .ConfigureAwait(false)))
            : current.Snapshot.Allocation.State
              == ZLinkPlacementAllocationState.Active
                ? new ZLinkObjectReserveResult.AlreadyExists(current.Snapshot)
                : new ZLinkObjectReserveResult.Conflict(
                    new ZLinkAuthorityReadResult.Found(current.Snapshot));
    }

    private static bool MatchesReservation(
        StoredAuthority? current,
        StoredRecord<ReservationRecord>? stored,
        ZLinkObjectReservation reservation) =>
        current is not null
        && stored is not null
        && stored.Record.Status == ReservationStatus.Reserved
        && stored.Record.Key == reservation.Key
        && stored.Record.ObjectGeneration == reservation.ObjectGeneration
        && stored.Record.AuthorityOwnerGeneration
        == reservation.AuthorityOwnerGeneration
        && stored.Record.ReservationId == reservation.ReservationVersion
        && stored.Record.TargetDescriptor == reservation.TargetDescriptor
        && stored.Record.TargetLifecycleGeneration
        == reservation.TargetNodeLifecycleGeneration
        && stored.Record.TargetOwner == reservation.TargetOwner
        && current.Version.Value == reservation.StoreVersion
        && current.Snapshot.Allocation.State
        == ZLinkPlacementAllocationState.Reserved;

    private static ZLinkAuthorityCompareExchangeResult Conflict(
        StoredAuthority? current) =>
        new ZLinkAuthorityCompareExchangeResult.Conflict(
            current is null
                ? new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow)
                : new ZLinkAuthorityReadResult.Found(current.Snapshot));

    private static ZLinkAuthoritySnapshot Snapshot(
        AuthorityMeta meta,
        ZLinkStoreVersion version,
        DateTimeOffset now,
        ReadOnlyMemory<byte> payload) =>
        new(
            version.Value,
            payload.ToArray(),
            meta.ObjectGeneration,
            meta.AuthorityOwnerGeneration,
            meta.OwnerId,
            meta.OwnerLeaseGeneration,
            meta.Allocation,
            meta.ReservedCreation,
            now);

    private static bool MatchesSourceAllocation(
        ZLinkPlacementAllocation allocation,
        ZLinkRelocationCapacityReservationRequest request) =>
        allocation.State == ZLinkPlacementAllocationState.Active
        && allocation.ObjectKind == request.ObjectKind
        && allocation.StableType == request.StableType
        && allocation.Descriptor == request.SourceDescriptor
        && allocation.DescriptorLifecycleGeneration
        == request.SourceNodeLifecycleGeneration
        && allocation.Capacity == request.Capacity;

    private static ZLinkPlacementAllocation TargetAllocation(
        ZLinkRelocationCapacityReservationRequest request) =>
        new(
            ZLinkPlacementAllocationState.Reserved,
            request.ObjectKind,
            request.StableType,
            request.TargetDescriptor,
            request.TargetNodeLifecycleGeneration,
            request.Capacity);

    private static bool IsEligible(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkPlacementAllocation allocation) =>
        descriptor.ObjectCapabilities.Any(capability =>
            capability.ObjectKind == allocation.ObjectKind
            && capability.StableType == allocation.StableType);

    private static bool HasCapacity(
        ZLinkMeshNodeDescriptor descriptor,
        CapacityRecord usage,
        ZLinkCapacityVector requested)
    {
        if (descriptor.Capacity.Actors.Limit > 0
            && usage.ActorsActive + usage.ActorsPending + requested.Actors
            > descriptor.Capacity.Actors.Limit)
            return false;
        if (descriptor.Capacity.Spots.Limit > 0
            && usage.SpotsActive + usage.SpotsPending + requested.Spots
            > descriptor.Capacity.Spots.Limit)
            return false;
        if (requested.SpotType is not { } spotType)
            return requested.Spots == 0;
        var limit = descriptor.ObjectCapabilities.SingleOrDefault(
            capability => capability.ObjectKind == spotType.ObjectKind
                          && capability.StableType == spotType.StableType)
            ?.Limit;
        if (limit is null) return false;
        var count = usage.SpotTypes.GetValueOrDefault(
            CapacityTypeKey(spotType.ObjectKind, spotType.StableType));
        return requested.Spots == spotType.Count
               && (limit == 0
                   || count.Active + count.Pending + spotType.Count <= limit);
    }

    private static void ApplyCapacity(
        CapacityRecord record,
        ZLinkPlacementAllocation allocation,
        int pendingDelta = 0,
        int activeDelta = 0) =>
        ApplyCapacityVector(
            record,
            allocation.Capacity,
            pendingDelta,
            activeDelta);

    private static void ApplyCapacityVector(
        CapacityRecord record,
        ZLinkCapacityVector vector,
        int pendingDelta = 0,
        int activeDelta = 0)
    {
        record.ActorsPending = checked(
            record.ActorsPending + vector.Actors * pendingDelta);
        record.ActorsActive = checked(
            record.ActorsActive + vector.Actors * activeDelta);
        record.SpotsPending = checked(
            record.SpotsPending + vector.Spots * pendingDelta);
        record.SpotsActive = checked(
            record.SpotsActive + vector.Spots * activeDelta);
        if (vector.SpotType is not { } spotType) return;
        var key = CapacityTypeKey(
            spotType.ObjectKind,
            spotType.StableType);
        var current = record.SpotTypes.GetValueOrDefault(key);
        record.SpotTypes[key] = new CapacityCount(
            checked(current.Active + spotType.Count * activeDelta),
            checked(current.Pending + spotType.Count * pendingDelta));
        RequireNonNegative(record);
    }

    private static void RequireNonNegative(CapacityRecord record)
    {
        if (record.ActorsActive < 0
            || record.ActorsPending < 0
            || record.SpotsActive < 0
            || record.SpotsPending < 0
            || record.SpotTypes.Values.Any(value =>
                value.Active < 0 || value.Pending < 0))
            throw new InvalidDataException(
                "The Location Store capacity record is inconsistent.");
    }

    private static void AddCondition(
        ICollection<ZLinkStoreCondition> conditions,
        ZLinkStoreCondition condition)
    {
        var key = condition switch
        {
            ZLinkStoreCondition.Missing value => value.Key,
            ZLinkStoreCondition.Version value => value.Key,
            _ => throw new ArgumentOutOfRangeException(nameof(condition))
        };
        if (conditions.Any(existing => existing switch
            {
                ZLinkStoreCondition.Missing value => value.Key == key,
                ZLinkStoreCondition.Version value => value.Key == key,
                _ => false
            }))
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

    private static void ValidateAuthorityMutation(ZLinkAuthorityMutation mutation)
    {
        if (mutation is not ZLinkAuthorityMutation.Put put) return;
        var preserve = put.GenerationTransition
                       == ZLinkAuthorityGenerationTransition.Preserve;
        var changesOwner = put.GenerationTransition
                           == ZLinkAuthorityGenerationTransition.NewOwner;
        if (!preserve && !changesOwner
            || preserve && put.TargetOwner is not null
            || changesOwner && (put.TargetOwner is null
                                || put.RelocationCapacityFence is null))
            throw new ArgumentException(
                "The authority mutation is inconsistent.",
                nameof(mutation));
    }

    private static void ValidateReservation(
        ZLinkObjectReservationRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateAuthorityKey(request.Key);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.StableType);
        ArgumentException.ThrowIfNullOrWhiteSpace(
            request.CreationIntentReference);
        if (request.CreationIntentHash.Length != 32
            || request.CreationIntentEncodedSize is < 0 or > 1024 * 1024
            || request.CreatingPayload.Length > 1024 * 1024
            || request.TargetNodeLifecycleGeneration == 0
            || request.TargetOwner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(request));
    }

    private static void ValidateRelocationRequest(
        ZLinkRelocationCapacityReservationRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.ReservationId == Guid.Empty
            || string.IsNullOrWhiteSpace(request.Key.Value)
            || string.IsNullOrWhiteSpace(request.ExpectedStoreVersion)
            || string.IsNullOrWhiteSpace(request.StableType)
            || request.SourceNodeLifecycleGeneration == 0
            || request.TargetNodeLifecycleGeneration == 0
            || request.SourceOwner.LeaseGeneration <= 0
            || request.TargetOwner.LeaseGeneration <= 0)
            throw new ArgumentException(
                "The relocation capacity reservation is invalid.",
                nameof(request));
    }

    private static void ValidateAggregate(ZLinkAggregatePrepareRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.AggregateId == Guid.Empty
            || request.AggregateGeneration is 0 or > long.MaxValue
            || request.Participants.Count < 1
            || request.InventoryDigest.Length != 32
            || request.TargetDescriptorLifecycleGeneration == 0
            || request.TargetOwner.LeaseGeneration <= 0
            || request.Participants.Select(value => value.Key.Value)
                .Distinct(StringComparer.Ordinal).Count()
            != request.Participants.Count)
            throw new ArgumentOutOfRangeException(nameof(request));
        if (request.Participants.Any(participant =>
                string.IsNullOrWhiteSpace(participant.Key.Value)
                || string.IsNullOrWhiteSpace(
                    participant.ExpectedStoreVersion)
                || participant.AuthorityPayload.Length > 1024 * 1024))
            throw new ArgumentOutOfRangeException(nameof(request));
    }

    private static void ValidateTerminal(
        ZLinkCreationTerminalPublication publication)
    {
        ValidateCreationOperation(publication.Operation);
        if (publication.TerminalEnvelope.Length > 1024 * 1024
            || publication.TerminalEnvelopeSha256.Length != 32)
            throw new ArgumentException(
                "The creation terminal publication is invalid.",
                nameof(publication));
        Span<byte> hash = stackalloc byte[32];
        SHA256.HashData(publication.TerminalEnvelope.Span, hash);
        if (!CryptographicOperations.FixedTimeEquals(
                hash,
                publication.TerminalEnvelopeSha256.Span))
            throw new ArgumentException(
                "The creation terminal checksum is invalid.",
                nameof(publication));
    }

    private static void ValidateCreationOperation(
        ZLinkCreationOperationId operation)
    {
        if (operation.SourceNodeRid.IsEmpty
            || operation.SourceNodeGeneration == 0
            || operation.OperationIdHigh == 0
            && operation.OperationIdLow == 0)
            throw new ArgumentOutOfRangeException(nameof(operation));
    }

    private static byte[] Sha256(ReadOnlyMemory<byte> payload) =>
        SHA256.HashData(payload.Span);

    private static byte[] Encode<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(
            value,
            ZLinkJsonSerializerOptions.Default);

    private static ZLinkStoreKey AuthorityMetaKey(ZLinkAuthorityKey key) =>
        Key(AuthorityMetaPrefix(key.Value));

    private static ZLinkStoreKey AuthorityPayloadKey(ZLinkAuthorityKey key) =>
        Key($"{AuthorityPrefix}payload:{EncodeSegment(key.Value)}");

    private static ZLinkStoreKey GenerationKey(ZLinkAuthorityKey key) =>
        Key($"{AuthorityPrefix}generation:{EncodeSegment(key.Value)}");

    private static ZLinkStoreKey AuthorityOwnerGenerationCounterKey() =>
        Key($"{AuthorityPrefix}owner-generation-counter");

    private static string AuthorityMetaPrefix(string prefix) =>
        $"{AuthorityMetaPrefixValue}{prefix}";

    private static string AuthorityMetaPrefixValue =>
        $"{AuthorityPrefix}meta:";

    private static ZLinkAuthorityKey DecodeAuthorityKey(ZLinkStoreKey key)
    {
        return new ZLinkAuthorityKey(
            key.Value[AuthorityMetaPrefixValue.Length..]);
    }

    private static ZLinkStoreKey ReservationKey(string reservationId) =>
        Key($"{ReservationPrefix}{EncodeSegment(reservationId)}");

    private static ZLinkStoreKey TerminalKey(
        ZLinkCreationOperationId operation) =>
        Key($"{TerminalPrefix}meta:{CreationOperationSegment(operation)}");

    private static ZLinkStoreKey TerminalPayloadKey(
        ZLinkCreationOperationId operation) =>
        Key($"{TerminalPrefix}payload:{CreationOperationSegment(operation)}");

    private static string CreationOperationSegment(
        ZLinkCreationOperationId operation) =>
        $"{operation.SourceNodeRid.ToHex()}:{operation.SourceNodeGeneration}:"
        + $"{operation.OperationIdHigh}:{operation.OperationIdLow}";

    private static ZLinkStoreKey RelocationKey(
        ZLinkRelocationCapacityFence fence) =>
        Key($"{RelocationCapacityPrefix}{EncodeSegment(fence.Value)}");

    private static ZLinkStoreKey AggregateKey(ZLinkAggregateFence fence) =>
        Key($"{AggregatePrefix}{fence.AggregateId:N}:"
            + fence.AggregateGeneration);

    private static ZLinkStoreKey AggregateParticipantMetaKey(
        ZLinkAggregateFence fence,
        int index) =>
        Key($"{AggregatePrefix}{fence.AggregateId:N}:"
            + $"{fence.AggregateGeneration}:participant:{index}:meta");

    private static ZLinkStoreKey AggregateParticipantPayloadKey(
        ZLinkAggregateFence fence,
        int index) =>
        Key($"{AggregatePrefix}{fence.AggregateId:N}:"
            + $"{fence.AggregateGeneration}:participant:{index}:payload");

    private static ZLinkStoreKey AggregateInventoryPageKey(
        ZLinkAggregateFence fence,
        int level,
        int index) =>
        Key($"{AggregatePrefix}{fence.AggregateId:N}:"
            + $"{fence.AggregateGeneration}:inventory:{level}:{index}");

    private static ZLinkStoreKey CapacityKey(
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong lifecycleGeneration) =>
        Key($"{Prefix}capacity:{EncodeSegment(descriptor.MeshName)}"
            + $"{EncodeSegment(descriptor.Rid.ToHex())}"
            + lifecycleGeneration);

    private static string CapacityTypeKey(
        ZLinkPlacementObjectKind kind,
        string stableType) =>
        $"{(int)kind}:{stableType}";

    private sealed record AuthorityMeta(
        byte[] PayloadSha256,
        ulong ObjectGeneration,
        ulong AuthorityOwnerGeneration,
        string OwnerId,
        long OwnerLeaseGeneration,
        ZLinkPlacementAllocation Allocation,
        ZLinkReservedObjectCreation? ReservedCreation)
    {
        public ZLinkAggregateFence? AggregateFence { get; init; }
        public int? AggregateParticipantIndex { get; init; }
        public string? AggregateExpectedStoreVersion { get; init; }
    }

    private sealed record StoredAuthority(
        ZLinkAuthorityKey Key,
        AuthorityMeta Meta,
        ZLinkStoreVersion Version,
        ZLinkAuthoritySnapshot Snapshot);

    private sealed record ReservationRecord(
        ZLinkAuthorityKey Key,
        ulong ObjectGeneration,
        ulong AuthorityOwnerGeneration,
        string ReservationId,
        ZLinkMeshNodeDescriptorKey TargetDescriptor,
        ulong TargetLifecycleGeneration,
        ZLinkLocationOwnerToken TargetOwner,
        ReservationStatus Status);

    private sealed record TerminalMeta(
        ZLinkCreationTerminalRecord Record,
        byte[] PayloadSha256);

    private sealed record StoredTerminal(
        ZLinkCreationTerminalRecord Record,
        ZLinkStoreVersion Version);

    private sealed record RelocationRecordState(
        ZLinkRelocationCapacityReservationRequest Request,
        RelocationStatus Status,
        ulong TargetAuthorityOwnerGeneration);

    private sealed record AggregateHeader(
        Guid AggregateId,
        ulong AggregateGeneration,
        byte[] InventoryDigest,
        ZLinkMeshNodeDescriptorKey TargetDescriptor,
        ulong TargetDescriptorLifecycleGeneration,
        ZLinkCapacityVector Capacity,
        ZLinkLocationOwnerToken TargetOwner);

    private sealed record AggregateRecord(
        AggregateHeader Header,
        int ParticipantCount,
        byte[] RequestFingerprint,
        AggregateInventoryRoot Inventory,
        AggregateStatus Status,
        DateTimeOffset StagedAt,
        bool ParticipantsNormalized,
        bool ParticipantsCleaned);

    private sealed record AggregateInventoryRoot(
        int TotalCount,
        byte[] Digest,
        int TopLevel,
        IReadOnlyList<AggregateInventoryPageReference> TopPages,
        IReadOnlyList<int> PageCountsByLevel);

    private sealed record AggregateInventoryPageReference(
        int Level,
        int Index,
        int StartIndex,
        int EntryCount,
        byte[] Sha256);

    private sealed record AggregateInventoryPage(
        int Level,
        int Index,
        int StartIndex,
        int EntryCount,
        IReadOnlyList<AggregateInventoryLeafEntry> Entries,
        IReadOnlyList<AggregateInventoryPageReference> Children);

    private sealed record AggregateInventoryLeafEntry(
        int Index,
        string Key,
        string ExpectedStoreVersion,
        ZLinkAuthorityGenerationTransition OwnerTransition,
        byte[] AuthorityPayloadSha256,
        byte[] MembershipMutationSha256);

    private sealed record AggregateInventoryTree(
        AggregateInventoryRoot Root,
        IReadOnlyList<AggregateInventoryPage> Pages);

    private ref struct AggregateInventoryReader
    {
        private ReadOnlySpan<byte> remaining;

        internal AggregateInventoryReader(ReadOnlySpan<byte> encoded)
        {
            remaining = encoded;
        }

        internal void ReadHeader(byte expectedRecordKind)
        {
            if (ReadUInt32() != InventoryCodecMagic
                || ReadByte() != InventoryCodecVersion
                || ReadByte() != expectedRecordKind)
                throw new InvalidDataException(
                    "The aggregate inventory codec header is invalid.");
        }

        internal int ReadInt32()
        {
            var bytes = ReadBytes(sizeof(int));
            return BinaryPrimitives.ReadInt32BigEndian(bytes);
        }

        internal int ReadBoundedCount(int maximum)
        {
            var count = ReadInt32();
            if (count < 0 || count > maximum)
                throw new InvalidDataException(
                    "An aggregate inventory collection count is invalid.");
            return count;
        }

        internal AggregateInventoryLeafEntry ReadLeafEntry()
        {
            var index = ReadInt32();
            var key = ReadString();
            var expectedStoreVersion = ReadString();
            var transition = (ZLinkAuthorityGenerationTransition)ReadByte();
            if (transition is not (
                    ZLinkAuthorityGenerationTransition.Preserve
                    or ZLinkAuthorityGenerationTransition.NewOwner))
                throw new InvalidDataException(
                    "An aggregate inventory owner transition is invalid.");
            return new AggregateInventoryLeafEntry(
                index,
                key,
                expectedStoreVersion,
                transition,
                ReadHash(),
                ReadHash());
        }

        internal AggregateInventoryPageReference ReadReference() =>
            new(
                ReadInt32(),
                ReadInt32(),
                ReadInt32(),
                ReadInt32(),
                ReadHash());

        internal void RequireEnd()
        {
            if (!remaining.IsEmpty)
                throw new InvalidDataException(
                    "The aggregate inventory record has trailing bytes.");
        }

        private uint ReadUInt32()
        {
            var bytes = ReadBytes(sizeof(uint));
            return BinaryPrimitives.ReadUInt32BigEndian(bytes);
        }

        private byte ReadByte() => ReadBytes(1)[0];

        private string ReadString()
        {
            var length = ReadInt32();
            if (length < 1 || length > remaining.Length)
                throw new InvalidDataException(
                    "An aggregate inventory string length is invalid.");
            return InventoryUtf8.GetString(ReadBytes(length));
        }

        private byte[] ReadHash() =>
            ReadBytes(SHA256.HashSizeInBytes).ToArray();

        private ReadOnlySpan<byte> ReadBytes(int count)
        {
            if (count < 0 || count > remaining.Length)
                throw new InvalidDataException(
                    "The aggregate inventory record is truncated.");
            var value = remaining[..count];
            remaining = remaining[count..];
            return value;
        }
    }

    private sealed record AggregateParticipantRecord(
        int Index,
        string Key,
        string ExpectedStoreVersion,
        ZLinkAuthorityGenerationTransition OwnerTransition,
        byte[] AuthorityPayloadSha256,
        byte[] MembershipMutationSha256,
        ulong TargetAuthorityOwnerGeneration,
        byte[] RequestFingerprint);

    private sealed record AggregateParticipantFingerprint(
        string Key,
        string ExpectedStoreVersion,
        ZLinkAuthorityGenerationTransition OwnerTransition,
        byte[] AuthorityPayloadSha256,
        byte[] MembershipMutationSha256);

    private sealed record StoredAggregateParticipant(
        AggregateParticipantRecord Metadata,
        ReadOnlyMemory<byte> Payload);

    private sealed record StoredRecord<T>(
        T Record,
        ZLinkStoreVersion Version,
        DateTimeOffset StoreNow);

    private sealed record StoredOwner(
        ZLinkLocationOwnerToken Token,
        ZLinkStoreVersion Version);

    private sealed record StoredTarget(
        ZLinkMeshNodeDescriptor Descriptor,
        ZLinkStoreCondition DescriptorCondition,
        ZLinkStoreCondition OwnerCondition);

    private sealed record StoredCapacity(
        ZLinkStoreKey Key,
        CapacityRecord Record,
        ZLinkStoreCondition Condition);

    private sealed record GenerationRecord(
        ulong ObjectGeneration,
        ulong AuthorityOwnerGeneration);

    private sealed record AuthorityOwnerGenerationCounter(ulong Value);

    private sealed record AuthorityOwnerGenerationCounterState(
        ulong Value,
        ZLinkStoreCondition Condition);

    private sealed record GenerationState(
        ulong ObjectGeneration,
        ulong AuthorityOwnerGeneration,
        ZLinkStoreCondition Condition);

    private sealed class CapacityRecord
    {
        public int ActorsActive { get; set; }
        public int ActorsPending { get; set; }
        public int SpotsActive { get; set; }
        public int SpotsPending { get; set; }
        public Dictionary<string, CapacityCount> SpotTypes { get; init; } =
            new(StringComparer.Ordinal);

        public CapacityRecord Clone() =>
            new()
            {
                ActorsActive = ActorsActive,
                ActorsPending = ActorsPending,
                SpotsActive = SpotsActive,
                SpotsPending = SpotsPending,
                SpotTypes = SpotTypes.ToDictionary(
                    static pair => pair.Key,
                    static pair => pair.Value,
                    StringComparer.Ordinal)
            };
    }

    private readonly record struct CapacityCount(int Active, int Pending);

    private enum ReservationStatus
    {
        Reserved = 1,
        Created = 2,
        Rejected = 3,
        Failed = 4,
        Aborted = 5
    }

    private enum StaleAuthorityReclaimResult
    {
        OwnerLive = 1,
        Reclaimed = 2,
        Conflict = 3,
        RecoveryRequired = 4
    }

    private enum RelocationStatus
    {
        Reserved = 1,
        Prepared = 2,
        Committed = 3,
        Aborted = 4
    }

    private enum AggregateStatus
    {
        Staging = 0,
        Prepared = 1,
        Committed = 2,
        Aborted = 3
    }
}
