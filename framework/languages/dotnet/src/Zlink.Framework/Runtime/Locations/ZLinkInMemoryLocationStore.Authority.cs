using System.Security.Cryptography;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkInMemoryLocationStore
{
    private readonly Dictionary<string, ZLinkAuthoritySnapshot> _authorities =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, AuthorityScan> _authorityScans =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, ReservationState> _authorityReservations =
        new(StringComparer.Ordinal);
    private readonly Dictionary<ZLinkCreationOperationId, ZLinkCreationTerminalRecord>
        _creationTerminals = [];
    private readonly Dictionary<string, RelocationCapacityState>
        _relocationCapacityReservations = new(StringComparer.Ordinal);
    private readonly Dictionary<ZLinkAggregateFence, AggregateState> _authorityAggregates = [];
    private readonly Dictionary<PlacementCapacityKey, long>
        _activePlacementCapacity = [];
    private readonly Dictionary<PlacementCapacityKey, long>
        _pendingPlacementCapacity = [];
    private long _authorityRevision;
    private long _authorityObjectGeneration;
    private long _authorityOwnerGeneration;

    public ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken = default)
    {
        ValidateAuthorityKey(key);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            return ValueTask.FromResult<ZLinkAuthorityReadResult>(
                _authorities.TryGetValue(key.Value, out var snapshot)
                    ? new ZLinkAuthorityReadResult.Found(
                        snapshot with { StoreNow = _time.GetUtcNow() })
                    : new ZLinkAuthorityReadResult.Missing(_time.GetUtcNow()));
        }
    }

    public ValueTask<ZLinkAuthorityCompareExchangeResult>
        CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default)
    {
        ValidateAuthorityKey(key);
        ArgumentException.ThrowIfNullOrWhiteSpace(expectedStoreVersion);
        ArgumentNullException.ThrowIfNull(mutation);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            _authorities.TryGetValue(key.Value, out var current);
            if (current is null
                || current.Allocation.State != ZLinkPlacementAllocationState.Active
                || !string.Equals(
                    current.StoreVersion,
                    expectedStoreVersion,
                    StringComparison.Ordinal)
                || IsAuthorityInPreparedAggregate(key))
            {
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Conflict(
                        current is null
                            ? new ZLinkAuthorityReadResult.Missing(now)
                            : new ZLinkAuthorityReadResult.Found(
                                current with { StoreNow = now })));
            }

            if (mutation is ZLinkAuthorityMutation.Restore restore)
            {
                ValidateAuthorityPayload(restore.Payload);
                if (string.IsNullOrWhiteSpace(restore.ExpectedOwner.OwnerId)
                    || restore.ExpectedOwner.LeaseGeneration <= 0)
                    throw new ArgumentOutOfRangeException(nameof(mutation));
                if (current.OwnerId != restore.ExpectedOwner.OwnerId
                    || current.OwnerLeaseGeneration
                    != restore.ExpectedOwner.LeaseGeneration)
                    return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                        new ZLinkAuthorityCompareExchangeResult.Conflict(
                            new ZLinkAuthorityReadResult.Found(
                                current with { StoreNow = now })));
                if (!CanIncrement(_authorityRevision))
                    return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                        new ZLinkAuthorityCompareExchangeResult.GenerationExhausted());
                var restored = current with
                {
                    StoreVersion = Next(ref _authorityRevision).ToString(),
                    Payload = restore.Payload.ToArray(),
                    StoreNow = now
                };
                _authorities[key.Value] = restored;
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Stored(restored));
            }

            if (mutation is ZLinkAuthorityMutation.Delete)
            {
                if (!MatchesLiveOwnerLease(
                        new ZLinkLocationOwnerToken(
                            current.OwnerId,
                            current.OwnerLeaseGeneration),
                        now))
                    return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                        new ZLinkAuthorityCompareExchangeResult.Conflict(
                            new ZLinkAuthorityReadResult.Found(
                                current with { StoreNow = now })));
                if (!CanIncrement(_authorityRevision))
                    return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                        new ZLinkAuthorityCompareExchangeResult.GenerationExhausted());
                var version = Next(ref _authorityRevision).ToString();
                AdjustAllocationCapacity(
                    _activePlacementCapacity,
                    current.Allocation,
                    -1);
                _authorities.Remove(key.Value);
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Deleted(version, now));
            }

            var put = (ZLinkAuthorityMutation.Put)mutation;
            ValidateAuthorityPayload(put.Payload);
            ValidateAuthorityMutation(put);
            var needsOwner = put.GenerationTransition
                             == ZLinkAuthorityGenerationTransition.NewOwner;
            var requiredOwner = needsOwner
                ? put.TargetOwner!.Value
                : new ZLinkLocationOwnerToken(
                    current.OwnerId,
                    current.OwnerLeaseGeneration);
            if (!MatchesLiveOwnerLease(requiredOwner, now))
            {
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Conflict(
                        current is null
                            ? new ZLinkAuthorityReadResult.Missing(now)
                            : new ZLinkAuthorityReadResult.Found(
                                current with { StoreNow = now })));
            }
            RelocationCapacityState? relocationCapacity = null;
            if (put.RelocationCapacityFence is { } relocationFence
                && (!_relocationCapacityReservations.TryGetValue(
                        relocationFence.Value,
                        out relocationCapacity)
                    || relocationCapacity.Status
                    != RelocationCapacityStatus.Reserved
                    || relocationCapacity.Request.Key != key
                    || relocationCapacity.Request.ExpectedStoreVersion
                    != current.StoreVersion
                    || relocationCapacity.Request.SourceOwner
                    != new ZLinkLocationOwnerToken(
                        current.OwnerId,
                        current.OwnerLeaseGeneration)
                    || needsOwner
                    && relocationCapacity.Request.TargetOwner
                       != put.TargetOwner.GetValueOrDefault()
                    || !MatchesSourceAllocation(
                        current.Allocation,
                        relocationCapacity.Request)
                    || !MatchesLiveTarget(
                        relocationCapacity.Request.TargetDescriptor,
                        relocationCapacity.Request.TargetNodeLifecycleGeneration,
                        relocationCapacity.Request.TargetOwner,
                        now)))
            {
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(
                            current! with { StoreNow = now })));
            }
            if (!CanIncrement(_authorityRevision))
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.GenerationExhausted());

            var owner = put.TargetOwner
                        ?? new ZLinkLocationOwnerToken(
                            current!.OwnerId,
                            current.OwnerLeaseGeneration);
            var stored = new ZLinkAuthoritySnapshot(
                Next(ref _authorityRevision).ToString(),
                put.Payload.ToArray(),
                current.ObjectGeneration,
                needsOwner
                    ? relocationCapacity!.TargetAuthorityOwnerGeneration
                    : current.AuthorityOwnerGeneration,
                owner.OwnerId,
                owner.LeaseGeneration,
                needsOwner
                    ? new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.Active,
                        relocationCapacity!.Request.ObjectKind,
                        relocationCapacity.Request.StableType,
                        relocationCapacity.Request.TargetDescriptor,
                        relocationCapacity.Request.TargetNodeLifecycleGeneration,
                        relocationCapacity.Request.Capacity)
                    : current.Allocation,
                current.ReservedCreation,
                now);
            _authorities[key.Value] = stored;
            if (put.RelocationCapacityFence is { } capacityFence)
            {
                if (needsOwner)
                {
                    MoveRelocationCapacity(
                        current.Allocation,
                        relocationCapacity!.Request);
                    _relocationCapacityReservations[capacityFence.Value].Status =
                        RelocationCapacityStatus.Committed;
                }
                else
                {
                    // Prepared publication and relocation capacity must become
                    // one durable fence. Rebind the reserved request to the
                    // StoreVersion produced by this same authority CAS so the
                    // following NewOwner CAS can consume the exact fence.
                    relocationCapacity!.Request = relocationCapacity.Request with
                    {
                        ExpectedStoreVersion = stored.StoreVersion
                    };
                }
            }
            return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                new ZLinkAuthorityCompareExchangeResult.Stored(stored));
        }
    }

    public ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
        string prefix,
        ZLinkAuthorityScanCursor? cursor,
        int limit,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(prefix);
        if (limit is < 1 or > 1000)
            throw new ArgumentOutOfRangeException(nameof(limit));
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            AuthorityScan scan;
            var position = 0;
            if (cursor is null)
            {
                var id = Guid.NewGuid().ToString("N");
                scan = new AuthorityScan(
                    _authorities
                        .Where(pair => pair.Key.StartsWith(
                            prefix,
                            StringComparison.Ordinal))
                        .OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                        .Select(pair => new ZLinkAuthorityEntry(
                            new ZLinkAuthorityKey(pair.Key),
                            pair.Value))
                        .ToArray(),
                    _time.GetUtcNow() + TimeSpan.FromMinutes(1));
                _authorityScans[id] = scan;
                cursor = new ZLinkAuthorityScanCursor($"{id}:0");
            }
            else
            {
                var separator = cursor.Value.Encoded.LastIndexOf(':');
                if (separator <= 0
                    || !int.TryParse(
                        cursor.Value.Encoded[(separator + 1)..],
                        out position)
                    || !_authorityScans.TryGetValue(
                        cursor.Value.Encoded[..separator],
                        out scan!)
                    || scan.ExpiresAt <= _time.GetUtcNow())
                {
                    return ValueTask.FromResult<ZLinkAuthorityScanResult>(
                        new ZLinkAuthorityScanResult.ScanExpired());
                }
            }

            var scanId = cursor.Value.Encoded[
                ..cursor.Value.Encoded.LastIndexOf(':')];
            var items = scan.Items.Skip(position).Take(limit).ToArray();
            var nextPosition = position + items.Length;
            ZLinkAuthorityScanCursor? next = nextPosition < scan.Items.Count
                ? new ZLinkAuthorityScanCursor($"{scanId}:{nextPosition}")
                : null;
            if (next is null)
                _authorityScans.Remove(scanId);
            return ValueTask.FromResult<ZLinkAuthorityScanResult>(
                new ZLinkAuthorityScanResult.Page(
                    new ZLinkAuthorityPage(items, next)));
        }
    }

    public ValueTask<ZLinkObjectReserveResult> ReserveAsync(
        ZLinkObjectReservationRequest request,
        CancellationToken cancellationToken = default)
    {
        ValidateReservationRequest(request);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            RemoveExpiredCreationTerminals(now);
            if (request.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot
                && Zlink.Framework.Runtime.Spots
                    .ZLinkUserSpotAuthorityPayloadCodec.TryGetSpotId(
                        request.Key,
                        out var spotId)
                && _entrySpotIdClaims.ContainsKey(spotId))
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    new ZLinkObjectReserveResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(now)));
            if (_authorities.TryGetValue(request.Key.Value, out var existing))
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    existing.Allocation.State == ZLinkPlacementAllocationState.Active
                        ? new ZLinkObjectReserveResult.AlreadyExists(existing)
                        : new ZLinkObjectReserveResult.Conflict(
                            new ZLinkAuthorityReadResult.Found(existing)));
            if (!MatchesLiveTarget(
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.TargetOwner,
                    now))
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    new ZLinkObjectReserveResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(now)));
            if (!TryGetEligibleTarget(
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.TargetOwner,
                    request.ObjectKind,
                    request.StableType,
                    now,
                    out var targetDescriptor))
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    new ZLinkObjectReserveResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(now)));
            if (!HasPlacementCapacity(
                    targetDescriptor,
                    request.Capacity))
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    new ZLinkObjectReserveResult.PlacementCapacityExhausted());
            if (!CanIncrement(_authorityRevision)
                || !CanIncrement(_authorityObjectGeneration)
                || !CanIncrement(_authorityOwnerGeneration))
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    new ZLinkObjectReserveResult.GenerationExhausted());

            var reservationVersion = Guid.NewGuid().ToString("N");
            var snapshot = new ZLinkAuthoritySnapshot(
                Next(ref _authorityRevision).ToString(),
                request.CreatingPayload.ToArray(),
                checked((ulong)Next(ref _authorityObjectGeneration)),
                checked((ulong)Next(ref _authorityOwnerGeneration)),
                request.TargetOwner.OwnerId,
                request.TargetOwner.LeaseGeneration,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Reserved,
                    request.ObjectKind,
                    request.StableType,
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.Capacity),
                new ZLinkReservedObjectCreation(
                    reservationVersion,
                    request.CreationIntentReference,
                    request.CreationIntentHash.ToArray(),
                    request.CreationIntentEncodedSize),
                now);
            _authorities[request.Key.Value] = snapshot;
            AdjustAllocationCapacity(
                _pendingPlacementCapacity,
                snapshot.Allocation,
                1);
            var reservation = new ZLinkObjectReservation(
                request.Key,
                snapshot.StoreVersion,
                snapshot.ObjectGeneration,
                snapshot.AuthorityOwnerGeneration,
                reservationVersion,
                request.TargetDescriptor,
                request.TargetNodeLifecycleGeneration,
                request.TargetOwner);
            _authorityReservations[reservationVersion] =
                new ReservationState(reservation, ReservationStatus.Reserved);
            return ValueTask.FromResult<ZLinkObjectReserveResult>(
                new ZLinkObjectReserveResult.Reserved(reservation));
        }
    }

    public ValueTask<ZLinkObjectCommitResult> CommitAsync(
        ZLinkObjectReservation reservation,
        ReadOnlyMemory<byte> readyPayload,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(reservation);
        ValidateAuthorityPayload(readyPayload);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!_authorityReservations.TryGetValue(
                    reservation.ReservationVersion,
                    out var state)
                || state.Reservation != reservation)
                return ValueTask.FromResult<ZLinkObjectCommitResult>(
                    new ZLinkObjectCommitResult.Stale());
            if (state.Status == ReservationStatus.Created)
                return ValueTask.FromResult<ZLinkObjectCommitResult>(
                    new ZLinkObjectCommitResult.AlreadyCommitted(state.Snapshot!));
            if (state.Status is ReservationStatus.Aborted
                or ReservationStatus.Rejected
                or ReservationStatus.Failed
                || !_authorities.TryGetValue(
                    reservation.Key.Value,
                    out var current)
                || current.StoreVersion != reservation.StoreVersion
                || current.Allocation.State
                != ZLinkPlacementAllocationState.Reserved)
                return ValueTask.FromResult<ZLinkObjectCommitResult>(
                    new ZLinkObjectCommitResult.Stale());
            var now = _time.GetUtcNow();
            if (!MatchesLiveTarget(
                    reservation.TargetDescriptor,
                    reservation.TargetNodeLifecycleGeneration,
                    reservation.TargetOwner,
                    now))
                return ValueTask.FromResult<ZLinkObjectCommitResult>(
                    new ZLinkObjectCommitResult.Stale());
            if (!CanIncrement(_authorityRevision))
                return ValueTask.FromResult<ZLinkObjectCommitResult>(
                    new ZLinkObjectCommitResult.GenerationExhausted());

            var stored = current with
            {
                StoreVersion = Next(ref _authorityRevision).ToString(),
                Payload = readyPayload.ToArray(),
                Allocation = current.Allocation with
                {
                    State = ZLinkPlacementAllocationState.Active
                },
                ReservedCreation = null,
                StoreNow = now
            };
            AdjustAllocationCapacity(
                _pendingPlacementCapacity,
                current.Allocation,
                -1);
            AdjustAllocationCapacity(
                _activePlacementCapacity,
                stored.Allocation,
                1);
            _authorities[reservation.Key.Value] = stored;
            state.Status = ReservationStatus.Created;
            state.Snapshot = stored;
            return ValueTask.FromResult<ZLinkObjectCommitResult>(
                new ZLinkObjectCommitResult.Committed(stored));
        }
    }

    public ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
        ZLinkObjectReservation reservation,
        ZLinkObjectCreationCompletion completion,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(reservation);
        ArgumentNullException.ThrowIfNull(completion);
        var publication = GetTerminalPublication(completion);
        ValidateTerminalPublication(reservation, publication);
        if (completion is ZLinkObjectCreationCompletion.Created created)
            ValidateAuthorityPayload(created.ReadyPayload);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            RemoveExpiredCreationTerminals(now);
            if (publication.ExpiresAt <= now)
                throw new ArgumentOutOfRangeException(
                    nameof(completion),
                    "The creation terminal must expire after StoreNow.");
            if (_creationTerminals.TryGetValue(
                    publication.Operation,
                    out var completed))
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.AlreadyCompleted(
                        completed with { StoreNow = now }));
            if (!_authorityReservations.TryGetValue(
                    reservation.ReservationVersion,
                    out var state)
                || state.Reservation != reservation)
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.Stale());
            if (state.Terminal is not null)
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.AlreadyCompleted(
                        state.Terminal with { StoreNow = now }));
            if (state.Status == ReservationStatus.Aborted
                || !_authorities.TryGetValue(
                    reservation.Key.Value,
                    out var current)
                || current.StoreVersion != reservation.StoreVersion
                || current.Allocation.State
                != ZLinkPlacementAllocationState.Reserved)
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.Stale());
            if (!MatchesLiveTarget(
                    reservation.TargetDescriptor,
                    reservation.TargetNodeLifecycleGeneration,
                reservation.TargetOwner,
                    now))
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.Stale());
            if (!CanIncrement(_authorityRevision))
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.GenerationExhausted());

            var terminalState = completion switch
            {
                ZLinkObjectCreationCompletion.Created =>
                    ZLinkCreationTerminalState.Created,
                ZLinkObjectCreationCompletion.Rejected =>
                    ZLinkCreationTerminalState.Rejected,
                ZLinkObjectCreationCompletion.Failed =>
                    ZLinkCreationTerminalState.Failed,
                _ => throw new ArgumentOutOfRangeException(nameof(completion))
            };
            var terminal = new ZLinkCreationTerminalRecord(
                publication.Operation,
                reservation.ReservationVersion,
                current.Allocation.ObjectKind,
                terminalState,
                publication.TerminalEnvelope.ToArray(),
                publication.TerminalEnvelopeSha256.ToArray(),
                publication.ExpiresAt,
                now);

            ZLinkAuthoritySnapshot? stored = null;
            if (completion is ZLinkObjectCreationCompletion.Created accepted)
            {
                stored = current with
                {
                    StoreVersion = Next(ref _authorityRevision).ToString(),
                    Payload = accepted.ReadyPayload.ToArray(),
                    Allocation = current.Allocation with
                    {
                        State = ZLinkPlacementAllocationState.Active
                    },
                    ReservedCreation = null,
                    StoreNow = now
                };
                AdjustAllocationCapacity(
                    _activePlacementCapacity,
                    stored.Allocation,
                    1);
                _authorities[reservation.Key.Value] = stored;
                state.Status = ReservationStatus.Created;
                state.Snapshot = stored;
            }
            else
            {
                _authorities.Remove(reservation.Key.Value);
                state.Status = completion is ZLinkObjectCreationCompletion.Rejected
                    ? ReservationStatus.Rejected
                    : ReservationStatus.Failed;
            }
            AdjustAllocationCapacity(
                _pendingPlacementCapacity,
                current.Allocation,
                -1);
            state.Terminal = terminal;
            _creationTerminals[publication.Operation] = terminal;

            return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                completion switch
                {
                    ZLinkObjectCreationCompletion.Created =>
                        new ZLinkObjectCreationCompleteResult.Created(stored!, terminal),
                    ZLinkObjectCreationCompletion.Rejected =>
                        new ZLinkObjectCreationCompleteResult.Rejected(terminal),
                    ZLinkObjectCreationCompletion.Failed =>
                        new ZLinkObjectCreationCompleteResult.Failed(terminal),
                    _ => throw new ArgumentOutOfRangeException(nameof(completion))
                });
        }
    }

    public ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
        ZLinkCreationOperationId operation,
        CancellationToken cancellationToken = default)
    {
        ValidateCreationOperation(operation);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            RemoveExpiredCreationTerminals(now);
            return ValueTask.FromResult<ZLinkCreationTerminalReadResult>(
                _creationTerminals.TryGetValue(operation, out var terminal)
                    ? new ZLinkCreationTerminalReadResult.Found(
                        terminal with { StoreNow = now })
                    : new ZLinkCreationTerminalReadResult.Missing(now));
        }
    }

    public ValueTask<ZLinkObjectAbortResult> AbortAsync(
        ZLinkObjectReservation reservation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(reservation);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!_authorityReservations.TryGetValue(
                    reservation.ReservationVersion,
                    out var state)
                || state.Reservation != reservation)
                return ValueTask.FromResult<ZLinkObjectAbortResult>(
                    new ZLinkObjectAbortResult.Stale());
            if (state.Status == ReservationStatus.Aborted)
                return ValueTask.FromResult<ZLinkObjectAbortResult>(
                    new ZLinkObjectAbortResult.AlreadyAborted());
            if (state.Status is ReservationStatus.Created
                or ReservationStatus.Rejected
                or ReservationStatus.Failed
                || !_authorities.TryGetValue(
                    reservation.Key.Value,
                    out var current)
                || current.StoreVersion != reservation.StoreVersion
                || current.Allocation.State
                != ZLinkPlacementAllocationState.Reserved)
                return ValueTask.FromResult<ZLinkObjectAbortResult>(
                    new ZLinkObjectAbortResult.Stale());
            if (!CanIncrement(_authorityRevision))
                return ValueTask.FromResult<ZLinkObjectAbortResult>(
                    new ZLinkObjectAbortResult.GenerationExhausted());

            Next(ref _authorityRevision);
            AdjustAllocationCapacity(
                _pendingPlacementCapacity,
                current.Allocation,
                -1);
            _authorities.Remove(reservation.Key.Value);
            state.Status = ReservationStatus.Aborted;
            return ValueTask.FromResult<ZLinkObjectAbortResult>(
                new ZLinkObjectAbortResult.Aborted());
        }
    }

    public ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
        ZLinkAggregatePrepareRequest request,
        CancellationToken cancellationToken = default)
    {
        ValidateAggregateRequest(request);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var fence = new ZLinkAggregateFence(
                request.AggregateId,
                request.AggregateGeneration);
            if (_authorityAggregates.TryGetValue(fence, out var existing))
            {
                return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                    (existing.Status is AggregateStatus.Prepared
                        or AggregateStatus.Committed)
                    && AggregateRequestsEqual(existing.Request, request)
                        ? new ZLinkAggregatePrepareResult.AlreadyPrepared(fence)
                        {
                            TargetAuthorityOwnerGenerations =
                                existing.TargetAuthorityOwnerGenerations
                        }
                        : new ZLinkAggregatePrepareResult.Conflict());
            }
            if (request.Participants.Any(participant =>
                    !_authorities.TryGetValue(
                        participant.Key.Value,
                        out var current)
                    || current.StoreVersion != participant.ExpectedStoreVersion))
                return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                    new ZLinkAggregatePrepareResult.Conflict());
            var now = _time.GetUtcNow();
            var relocating = request.Participants
                .Where(static participant =>
                    participant.OwnerTransition
                    == ZLinkAuthorityGenerationTransition.NewOwner)
                .ToArray();
            if (!MatchesLiveTarget(
                    request.TargetDescriptor,
                    request.TargetDescriptorLifecycleGeneration,
                    request.TargetOwner,
                    now)
                || !AggregateCapacityMatchesParticipants(request, relocating)
                || relocating.Any(participant =>
                    !_authorities.TryGetValue(participant.Key.Value, out var source)
                    || !TryGetEligibleTarget(
                        request.TargetDescriptor,
                        request.TargetDescriptorLifecycleGeneration,
                        request.TargetOwner,
                        source.Allocation.ObjectKind,
                        source.Allocation.StableType,
                        now,
                        out _,
                        request.AllowPreparingTarget))
                || IsParticipantInPreparedAggregate(request.Participants)
                || !TryGetTargetDescriptor(
                    request.TargetDescriptor,
                    request.TargetDescriptorLifecycleGeneration,
                    out var targetDescriptor)
                || !HasPlacementCapacity(targetDescriptor, request.Capacity))
                return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                    new ZLinkAggregatePrepareResult.Conflict());

            if (_authorityOwnerGeneration
                > long.MaxValue - relocating.Length)
                return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                    new ZLinkAggregatePrepareResult.GenerationExhausted());
            var targetAuthorityOwnerGenerations =
                new Dictionary<ZLinkAuthorityKey, ulong>(
                    request.Participants.Count);
            foreach (var participant in request.Participants)
            {
                var current = _authorities[participant.Key.Value];
                targetAuthorityOwnerGenerations[participant.Key] =
                    participant.OwnerTransition
                    == ZLinkAuthorityGenerationTransition.NewOwner
                        ? checked((ulong)Next(ref _authorityOwnerGeneration))
                        : current.AuthorityOwnerGeneration;
            }
            _authorityAggregates[fence] =
                new AggregateState(
                    CloneAggregateRequest(request),
                    AggregateStatus.Prepared,
                    targetAuthorityOwnerGenerations);
            foreach (var participant in relocating)
            {
                var source = _authorities[participant.Key.Value].Allocation;
                AdjustAllocationCapacity(
                    _pendingPlacementCapacity,
                    source with
                    {
                        Descriptor = request.TargetDescriptor,
                        DescriptorLifecycleGeneration =
                            request.TargetDescriptorLifecycleGeneration
                    },
                    1);
            }
            return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                new ZLinkAggregatePrepareResult.Prepared(fence)
                {
                    TargetAuthorityOwnerGenerations =
                        targetAuthorityOwnerGenerations
                });
        }
    }

    public ValueTask<ZLinkRelocationCapacityReserveResult>
        ReserveRelocationCapacityAsync(
            ZLinkRelocationCapacityReservationRequest request,
            CancellationToken cancellationToken = default)
    {
        ValidateRelocationCapacityRequest(request);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var fence = new ZLinkRelocationCapacityFence(
                request.ReservationId.ToString("N"));
            if (_relocationCapacityReservations.TryGetValue(
                    fence.Value,
                    out var existing))
            {
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    existing.Status
                    == RelocationCapacityStatus.Reserved
                    && existing.Request == request
                        ? new ZLinkRelocationCapacityReserveResult.AlreadyReserved(
                            fence)
                        {
                            TargetAuthorityOwnerGeneration =
                                existing.TargetAuthorityOwnerGeneration
                        }
                        : new ZLinkRelocationCapacityReserveResult.Conflict(
                            ReadCurrent(request.Key)));
            }
            if (!_authorities.TryGetValue(request.Key.Value, out var current)
                || current.StoreVersion != request.ExpectedStoreVersion
                || current.OwnerId != request.SourceOwner.OwnerId
                || current.OwnerLeaseGeneration
                != request.SourceOwner.LeaseGeneration
                || !MatchesSourceAllocation(current.Allocation, request)
                || IsAuthorityInPreparedAggregate(request.Key))
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    new ZLinkRelocationCapacityReserveResult.Conflict(
                        ReadCurrent(request.Key)));
            if (!MatchesLiveTarget(
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.TargetOwner,
                    _time.GetUtcNow()))
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    new ZLinkRelocationCapacityReserveResult.TargetUnavailable());
            if (!TryGetEligibleTarget(
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.TargetOwner,
                    request.ObjectKind,
                    request.StableType,
                    _time.GetUtcNow(),
                    out var targetDescriptor))
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    new ZLinkRelocationCapacityReserveResult.TargetUnavailable());
            if (!HasPlacementCapacity(
                    targetDescriptor,
                    request.Capacity))
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    new ZLinkRelocationCapacityReserveResult
                        .PlacementCapacityExhausted());
            if (!CanIncrement(_authorityOwnerGeneration))
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "reserving relocation capacity");
            var targetAuthorityOwnerGeneration =
                checked((ulong)Next(ref _authorityOwnerGeneration));
            _relocationCapacityReservations[fence.Value] =
                new RelocationCapacityState(
                    request,
                    RelocationCapacityStatus.Reserved,
                    targetAuthorityOwnerGeneration);
            AdjustAllocationCapacity(
                _pendingPlacementCapacity,
                TargetAllocation(request),
                1);
            return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                new ZLinkRelocationCapacityReserveResult.Reserved(fence)
                {
                    TargetAuthorityOwnerGeneration =
                        targetAuthorityOwnerGeneration
                });
        }
    }

    public ValueTask<ZLinkRelocationCapacityAbortResult>
        AbortRelocationCapacityAsync(
            ZLinkRelocationCapacityFence fence,
            CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(fence.Value);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!_relocationCapacityReservations.TryGetValue(
                    fence.Value,
                    out var state))
                return ValueTask.FromResult(
                    ZLinkRelocationCapacityAbortResult.Stale);
            if (state.Status == RelocationCapacityStatus.Committed)
                return ValueTask.FromResult(
                    ZLinkRelocationCapacityAbortResult.AlreadyCommitted);
            if (state.Status == RelocationCapacityStatus.Aborted)
                return ValueTask.FromResult(
                    ZLinkRelocationCapacityAbortResult.AlreadyAborted);
            if (state.Status == RelocationCapacityStatus.Prepared)
                return ValueTask.FromResult(
                    ZLinkRelocationCapacityAbortResult.Stale);
            state.Status = RelocationCapacityStatus.Aborted;
            AdjustAllocationCapacity(
                _pendingPlacementCapacity,
                TargetAllocation(state.Request),
                -1);
            return ValueTask.FromResult(
                ZLinkRelocationCapacityAbortResult.Aborted);
        }
    }

    private ZLinkAuthorityReadResult ReadCurrent(ZLinkAuthorityKey key)
    {
        var now = _time.GetUtcNow();
        return _authorities.TryGetValue(key.Value, out var current)
            ? new ZLinkAuthorityReadResult.Found(
                current with { StoreNow = now })
            : new ZLinkAuthorityReadResult.Missing(now);
    }

    public ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!_authorityAggregates.TryGetValue(fence, out var aggregate)
                || aggregate.Status == AggregateStatus.Aborted)
                return ValueTask.FromResult(ZLinkAggregateCommitResult.Stale);
            if (aggregate.Status == AggregateStatus.Committed)
                return ValueTask.FromResult(
                    ZLinkAggregateCommitResult.AlreadyCommitted);

            if (_authorityRevision > long.MaxValue
                    - aggregate.Request.Participants.Count)
                return ValueTask.FromResult(
                    ZLinkAggregateCommitResult.GenerationExhausted);
            if (aggregate.Request.Participants.Any(participant =>
                    !_authorities.TryGetValue(
                        participant.Key.Value,
                        out var current)
                    || current.StoreVersion != participant.ExpectedStoreVersion))
                return ValueTask.FromResult(ZLinkAggregateCommitResult.Stale);
            var now = _time.GetUtcNow();
            if (!MatchesLiveTarget(
                    aggregate.Request.TargetDescriptor,
                    aggregate.Request.TargetDescriptorLifecycleGeneration,
                    aggregate.Request.TargetOwner,
                    now))
                return ValueTask.FromResult(ZLinkAggregateCommitResult.Stale);

            foreach (var participant in aggregate.Request.Participants)
            {
                var current = _authorities[participant.Key.Value];
                var changesOwner = participant.OwnerTransition
                                   == ZLinkAuthorityGenerationTransition.NewOwner;
                var stored = current with
                {
                    StoreVersion = Next(ref _authorityRevision).ToString(),
                    Payload = participant.AuthorityPayload.ToArray(),
                    AuthorityOwnerGeneration = changesOwner
                        ? aggregate.TargetAuthorityOwnerGenerations[
                            participant.Key]
                        : current.AuthorityOwnerGeneration,
                    OwnerId = changesOwner
                        ? aggregate.Request.TargetOwner.OwnerId
                        : current.OwnerId,
                    OwnerLeaseGeneration = changesOwner
                        ? aggregate.Request.TargetOwner.LeaseGeneration
                        : current.OwnerLeaseGeneration,
                    Allocation = changesOwner
                        ? current.Allocation with
                        {
                            Descriptor = aggregate.Request.TargetDescriptor,
                            DescriptorLifecycleGeneration =
                                aggregate.Request
                                    .TargetDescriptorLifecycleGeneration
                        }
                        : current.Allocation,
                    StoreNow = now
                };
                if (changesOwner)
                {
                    AdjustAllocationCapacity(
                        _pendingPlacementCapacity,
                        stored.Allocation,
                        -1);
                    AdjustAllocationCapacity(
                        _activePlacementCapacity,
                        current.Allocation,
                        -1);
                    AdjustAllocationCapacity(
                        _activePlacementCapacity,
                        stored.Allocation,
                        1);
                }
                _authorities[participant.Key.Value] = stored;
            }
            aggregate.Status = AggregateStatus.Committed;
            return ValueTask.FromResult(ZLinkAggregateCommitResult.Committed);
        }
    }

    public ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!_authorityAggregates.TryGetValue(fence, out var aggregate))
                return ValueTask.FromResult(ZLinkAggregateAbortResult.Stale);
            if (aggregate.Status == AggregateStatus.Aborted)
                return ValueTask.FromResult(
                    ZLinkAggregateAbortResult.AlreadyAborted);
            if (aggregate.Status == AggregateStatus.Committed)
                return ValueTask.FromResult(ZLinkAggregateAbortResult.Stale);
            foreach (var participant in aggregate.Request.Participants.Where(
                         static participant =>
                             participant.OwnerTransition
                             == ZLinkAuthorityGenerationTransition.NewOwner))
            {
                var source = _authorities[participant.Key.Value].Allocation;
                AdjustAllocationCapacity(
                    _pendingPlacementCapacity,
                    source with
                    {
                        Descriptor = aggregate.Request.TargetDescriptor,
                        DescriptorLifecycleGeneration =
                            aggregate.Request
                                .TargetDescriptorLifecycleGeneration
                    },
                    -1);
            }
            aggregate.Status = AggregateStatus.Aborted;
            return ValueTask.FromResult(ZLinkAggregateAbortResult.Aborted);
        }
    }

    private bool MatchesLiveTarget(
        ZLinkMeshNodeDescriptorKey descriptorKey,
        ulong lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        DateTimeOffset now)
    {
        if (!MatchesLiveOwnerLease(owner, now))
            return false;
        var encoded = ZLinkLocationKeyCodec.EncodeMeshNodeKey(descriptorKey);
        return _meshNodes.Rows.TryGetValue(encoded, out var descriptor)
               && descriptor.LifecycleGeneration == lifecycleGeneration
               && string.Equals(
                   descriptor.OwnerId,
                   owner.OwnerId,
                   StringComparison.Ordinal)
               && descriptor.LeaseGeneration == owner.LeaseGeneration;
    }

    private bool TryGetTargetDescriptor(
        ZLinkMeshNodeDescriptorKey descriptorKey,
        ulong lifecycleGeneration,
        out ZLinkMeshNodeDescriptor descriptor)
    {
        var encoded = ZLinkLocationKeyCodec.EncodeMeshNodeKey(descriptorKey);
        return _meshNodes.Rows.TryGetValue(encoded, out descriptor!)
               && descriptor.LifecycleGeneration == lifecycleGeneration;
    }

    private bool TryGetEligibleTarget(
        ZLinkMeshNodeDescriptorKey descriptorKey,
        ulong lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind objectKind,
        string stableType,
        DateTimeOffset now,
        out ZLinkMeshNodeDescriptor descriptor,
        bool allowPreparingTarget = false)
    {
        descriptor = null!;
        if (!MatchesLiveTarget(
                descriptorKey,
                lifecycleGeneration,
                owner,
                now))
            return false;
        var encoded = ZLinkLocationKeyCodec.EncodeMeshNodeKey(descriptorKey);
        descriptor = _meshNodes.Rows[encoded];
        if (descriptor.ObjectRole != ZLinkMeshNodeObjectRole.Server
            || descriptor.State != ZLinkFrameworkRuntimeState.Serving
            && !(allowPreparingTarget
                 && descriptor.State == ZLinkFrameworkRuntimeState.Preparing)
            || descriptor.PlacementWeight <= 0)
            return false;
        var capability = descriptor.ObjectCapabilities.SingleOrDefault(
            value => value.ObjectKind == objectKind
                     && string.Equals(
                         value.StableType,
                         stableType,
                         StringComparison.Ordinal));
        return capability is not null;
    }

    private bool HasPlacementCapacity(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkCapacityVector capacity)
    {
        var descriptorKey = new ZLinkMeshNodeDescriptorKey(
            descriptor.MeshName,
            descriptor.Rid);
        var actorActive = PlacementCapacityUsage(
            _activePlacementCapacity,
            descriptorKey,
            descriptor.LifecycleGeneration,
            ZLinkPlacementObjectKind.Actor);
        var actorReserved = PlacementCapacityUsage(
            _pendingPlacementCapacity,
            descriptorKey,
            descriptor.LifecycleGeneration,
            ZLinkPlacementObjectKind.Actor);
        var spotKinds = new[]
        {
            ZLinkPlacementObjectKind.UserSpot,
            ZLinkPlacementObjectKind.InstanceSpot
        };
        var spotActive = PlacementCapacityUsage(
            _activePlacementCapacity,
            descriptorKey,
            descriptor.LifecycleGeneration,
            spotKinds);
        var spotReserved = PlacementCapacityUsage(
            _pendingPlacementCapacity,
            descriptorKey,
            descriptor.LifecycleGeneration,
            spotKinds);
        if (!HasCapacity(
                actorActive,
                actorReserved,
                capacity.Actors,
                descriptor.Capacity.Actors.Limit)
            || !HasCapacity(
                spotActive,
                spotReserved,
                capacity.Spots,
                descriptor.Capacity.Spots.Limit))
            return false;

        if (capacity.SpotType is not { } spotType)
            return capacity.Spots == 0;
        var capability = descriptor.ObjectCapabilities.SingleOrDefault(
            value => value.ObjectKind == spotType.ObjectKind
                     && string.Equals(
                         value.StableType,
                         spotType.StableType,
                         StringComparison.Ordinal));
        if (capability is null)
            return false;
        var typeKey = new PlacementCapacityKey(
            descriptorKey,
            descriptor.LifecycleGeneration,
            spotType.ObjectKind,
            spotType.StableType);
        return capacity.Spots == spotType.Count
               && HasCapacity(
                   _activePlacementCapacity.GetValueOrDefault(typeKey),
                   _pendingPlacementCapacity.GetValueOrDefault(typeKey),
                   spotType.Count,
                   capability.Limit);
    }

    private static long PlacementCapacityUsage(
        IReadOnlyDictionary<PlacementCapacityKey, long> counters,
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong lifecycleGeneration,
        params ZLinkPlacementObjectKind[] objectKinds) =>
        counters
            .Where(pair =>
                pair.Key.Descriptor == descriptor
                && pair.Key.DescriptorLifecycleGeneration
                == lifecycleGeneration
                && objectKinds.Contains(pair.Key.ObjectKind))
            .Sum(static pair => pair.Value);

    private static bool HasCapacity(
        long active,
        long reserved,
        int delta,
        int limit) =>
        limit == 0 || active + reserved <= limit - (long)delta;

    private bool IsParticipantInPreparedAggregate(
        IReadOnlyList<ZLinkAggregateParticipant> participants)
    {
        var keys = participants
            .Select(static participant => participant.Key)
            .ToHashSet();
        return _authorityAggregates.Values.Any(aggregate =>
            aggregate.Status == AggregateStatus.Prepared
            && aggregate.Request.Participants.Any(participant =>
                keys.Contains(participant.Key)));
    }

    private bool IsAuthorityInPreparedAggregate(ZLinkAuthorityKey key) =>
        _authorityAggregates.Values.Any(aggregate =>
            aggregate.Status == AggregateStatus.Prepared
            && aggregate.Request.Participants.Any(participant =>
                participant.Key == key));

    private bool AggregateCapacityMatchesParticipants(
        ZLinkAggregatePrepareRequest request,
        IReadOnlyList<ZLinkAggregateParticipant> relocating)
    {
        long actors = 0;
        long spots = 0;
        ZLinkSpotTypeCapacityDelta? spotType = null;
        foreach (var participant in relocating)
        {
            var allocation = _authorities[participant.Key.Value].Allocation;
            actors = checked(actors + allocation.Capacity.Actors);
            spots = checked(spots + allocation.Capacity.Spots);
            if (allocation.Capacity.SpotType is not { } participantSpotType)
                continue;
            if (spotType is null)
                spotType = participantSpotType;
            else if (spotType.ObjectKind != participantSpotType.ObjectKind
                     || !string.Equals(
                         spotType.StableType,
                         participantSpotType.StableType,
                         StringComparison.Ordinal))
                return false;
            else
                spotType = spotType with
                {
                    Count = checked(spotType.Count + participantSpotType.Count)
                };
        }

        return actors == request.Capacity.Actors
               && spots == request.Capacity.Spots
               && spotType == request.Capacity.SpotType;
    }

    private static bool IsCapacityVectorValid(ZLinkCapacityVector capacity)
    {
        ArgumentNullException.ThrowIfNull(capacity);
        if (capacity.Actors < 0
            || capacity.Spots < 0
            || capacity.Actors == 0 && capacity.Spots == 0)
            return false;
        if (capacity.SpotType is not { } spotType)
            return capacity.Spots == 0;
        return capacity.Spots == spotType.Count
               && spotType.Count > 0
               && spotType.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                   or ZLinkPlacementObjectKind.InstanceSpot
               && !string.IsNullOrWhiteSpace(spotType.StableType);
    }

    private static bool IsAllocationCapacityValid(
        ZLinkPlacementObjectKind objectKind,
        string stableType,
        ZLinkCapacityVector capacity)
    {
        if (!IsCapacityVectorValid(capacity))
            return false;
        return objectKind switch
        {
            ZLinkPlacementObjectKind.Actor =>
                capacity.Actors == 1
                && capacity.Spots == 0
                && capacity.SpotType is null,
            ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot =>
                capacity.Actors == 0
                && capacity.Spots == 1
                && capacity.SpotType is
                {
                    Count: 1
                } spotType
                && spotType.ObjectKind == objectKind
                && string.Equals(
                    spotType.StableType,
                    stableType,
                    StringComparison.Ordinal),
            _ => false
        };
    }

    private static bool MatchesSourceAllocation(
        ZLinkPlacementAllocation allocation,
        ZLinkRelocationCapacityReservationRequest request) =>
        allocation.State == ZLinkPlacementAllocationState.Active
        && allocation.ObjectKind == request.ObjectKind
        && string.Equals(
            allocation.StableType,
            request.StableType,
            StringComparison.Ordinal)
        && allocation.Descriptor == request.SourceDescriptor
        && allocation.DescriptorLifecycleGeneration
        == request.SourceNodeLifecycleGeneration
        && allocation.Capacity == request.Capacity;

    internal (long Pending, long Active) GetPlacementCapacityUsage(
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong descriptorLifecycleGeneration,
        ZLinkPlacementObjectKind objectKind,
        string stableType)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(stableType);
        var key = new PlacementCapacityKey(
            descriptor,
            descriptorLifecycleGeneration,
            objectKind,
            stableType);
        lock (_gate)
        {
            return (
                _pendingPlacementCapacity.GetValueOrDefault(key),
                _activePlacementCapacity.GetValueOrDefault(key));
        }
    }

    private void MoveRelocationCapacity(
        ZLinkPlacementAllocation source,
        ZLinkRelocationCapacityReservationRequest request)
    {
        AdjustAllocationCapacity(
            _activePlacementCapacity,
            source,
            -1);
        AdjustAllocationCapacity(
            _pendingPlacementCapacity,
            TargetAllocation(request),
            -1);
        AdjustAllocationCapacity(
            _activePlacementCapacity,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Active,
                request.ObjectKind,
                request.StableType,
                request.TargetDescriptor,
                request.TargetNodeLifecycleGeneration,
                request.Capacity),
            1);
    }

    private void AdjustAllocationCapacity(
        Dictionary<PlacementCapacityKey, long> counters,
        ZLinkPlacementAllocation allocation,
        int multiplier)
    {
        if (allocation.Capacity.Actors != 0)
            AdjustPlacementCapacity(
                counters,
                PlacementCapacityKey.From(allocation),
                checked((long)allocation.Capacity.Actors * multiplier));
        if (allocation.Capacity.SpotType is { } spotType)
            AdjustPlacementCapacity(
                counters,
                new PlacementCapacityKey(
                    allocation.Descriptor,
                    allocation.DescriptorLifecycleGeneration,
                    spotType.ObjectKind,
                    spotType.StableType),
                checked((long)spotType.Count * multiplier));
    }

    private static ZLinkPlacementAllocation TargetAllocation(
        ZLinkRelocationCapacityReservationRequest request) =>
        new(
            ZLinkPlacementAllocationState.Reserved,
            request.ObjectKind,
            request.StableType,
            request.TargetDescriptor,
            request.TargetNodeLifecycleGeneration,
            request.Capacity);

    private void AdjustCapacityVector(
        Dictionary<PlacementCapacityKey, long> counters,
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong descriptorLifecycleGeneration,
        ZLinkCapacityVector capacity,
        int multiplier)
    {
        if (capacity.Actors != 0)
            AdjustPlacementCapacity(
                counters,
                new PlacementCapacityKey(
                    descriptor,
                    descriptorLifecycleGeneration,
                    ZLinkPlacementObjectKind.Actor,
                    string.Empty),
                checked((long)capacity.Actors * multiplier));
        if (capacity.SpotType is { } spotType)
            AdjustPlacementCapacity(
                counters,
                new PlacementCapacityKey(
                    descriptor,
                    descriptorLifecycleGeneration,
                    spotType.ObjectKind,
                    spotType.StableType),
                checked((long)spotType.Count * multiplier));
    }

    private void AdjustPlacementCapacity(
        Dictionary<PlacementCapacityKey, long> counters,
        PlacementCapacityKey key,
        long delta)
    {
        var next = checked(counters.GetValueOrDefault(key) + delta);
        if (next < 0)
            throw new InvalidOperationException(
                "The in-memory placement capacity counter underflowed.");
        if (next == 0)
            counters.Remove(key);
        else
            counters[key] = next;

        var encoded = ZLinkLocationKeyCodec.EncodeMeshNodeKey(key.Descriptor);
        if (_meshNodes.Rows.TryGetValue(encoded, out var descriptor)
            && descriptor.LifecycleGeneration
            == key.DescriptorLifecycleGeneration)
            _meshNodes.Rows[encoded] =
                WithCurrentPlacementCapacity(descriptor);
    }

    private static bool AggregateRequestsEqual(
        ZLinkAggregatePrepareRequest left,
        ZLinkAggregatePrepareRequest right)
    {
        if (left.AggregateId != right.AggregateId
            || left.AggregateGeneration != right.AggregateGeneration
            || left.TargetOwner != right.TargetOwner
            || left.TargetDescriptor != right.TargetDescriptor
            || left.TargetDescriptorLifecycleGeneration
            != right.TargetDescriptorLifecycleGeneration
            || left.Capacity != right.Capacity
            || left.AllowPreparingTarget != right.AllowPreparingTarget
            || !left.InventoryDigest.Span.SequenceEqual(
                right.InventoryDigest.Span)
            || left.Participants.Count != right.Participants.Count)
            return false;

        for (var index = 0; index < left.Participants.Count; index++)
        {
            var leftParticipant = left.Participants[index];
            var rightParticipant = right.Participants[index];
            if (leftParticipant.Key != rightParticipant.Key
                || !string.Equals(
                    leftParticipant.ExpectedStoreVersion,
                    rightParticipant.ExpectedStoreVersion,
                    StringComparison.Ordinal)
                || leftParticipant.OwnerTransition
                != rightParticipant.OwnerTransition
                || !leftParticipant.AuthorityPayload.Span.SequenceEqual(
                    rightParticipant.AuthorityPayload.Span)
                || !leftParticipant.MembershipMutation.Span.SequenceEqual(
                    rightParticipant.MembershipMutation.Span))
                return false;
        }

        return true;
    }

    private static ZLinkAggregatePrepareRequest CloneAggregateRequest(
        ZLinkAggregatePrepareRequest request) =>
        request with
        {
            Participants = request.Participants.Select(
                static participant => participant with
                {
                    AuthorityPayload = participant.AuthorityPayload.ToArray(),
                    MembershipMutation =
                        participant.MembershipMutation.ToArray()
                }).ToArray(),
            InventoryDigest = request.InventoryDigest.ToArray()
        };

    private static void ValidateAuthorityKey(ZLinkAuthorityKey key) =>
        ArgumentException.ThrowIfNullOrWhiteSpace(key.Value);

    private static void ValidateAuthorityPayload(ReadOnlyMemory<byte> payload)
    {
        if (payload.Length > 1024 * 1024)
            throw new ArgumentOutOfRangeException(nameof(payload));
    }

    private static void ValidateAuthorityMutation(
        ZLinkAuthorityMutation.Put put)
    {
        var preserve =
            put.GenerationTransition == ZLinkAuthorityGenerationTransition.Preserve;
        var newOwner =
            put.GenerationTransition == ZLinkAuthorityGenerationTransition.NewOwner;
        if (!preserve && !newOwner)
            throw new ArgumentOutOfRangeException(nameof(put));
        if (preserve && put.TargetOwner is not null
            || newOwner && (put.TargetOwner is null
                            || put.RelocationCapacityFence is null)
            || put.TargetOwner is { } owner
            && (string.IsNullOrWhiteSpace(owner.OwnerId)
                || owner.LeaseGeneration <= 0))
            throw new ArgumentException(
                "Authority mutation transition, expectation, and target owner are inconsistent.",
                nameof(put));
    }

    private static void ValidateRelocationCapacityRequest(
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
            || request.TargetOwner.LeaseGeneration <= 0
            || !IsAllocationCapacityValid(
                request.ObjectKind,
                request.StableType,
                request.Capacity))
            throw new ArgumentException(
                "The relocation capacity reservation is invalid.",
                nameof(request));
    }

    private static void ValidateReservationRequest(
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
            || !IsAllocationCapacityValid(
                request.ObjectKind,
                request.StableType,
                request.Capacity)
            || request.TargetNodeLifecycleGeneration == 0
            || request.TargetOwner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(request));
    }

    private static ZLinkCreationTerminalPublication GetTerminalPublication(
        ZLinkObjectCreationCompletion completion) =>
        completion switch
        {
            ZLinkObjectCreationCompletion.Created created => created.Terminal,
            ZLinkObjectCreationCompletion.Rejected rejected => rejected.Terminal,
            ZLinkObjectCreationCompletion.Failed failed => failed.Terminal,
            _ => throw new ArgumentOutOfRangeException(nameof(completion))
        };

    private static void ValidateTerminalPublication(
        ZLinkObjectReservation reservation,
        ZLinkCreationTerminalPublication publication)
    {
        ArgumentNullException.ThrowIfNull(publication);
        ValidateCreationOperation(publication.Operation);
        if (publication.TerminalEnvelope.Length > 1024 * 1024
            || publication.TerminalEnvelopeSha256.Length != 32)
            throw new ArgumentException(
                "The creation terminal publication does not match its reservation.",
                nameof(publication));
        Span<byte> digest = stackalloc byte[32];
        SHA256.HashData(publication.TerminalEnvelope.Span, digest);
        if (!CryptographicOperations.FixedTimeEquals(
                digest,
                publication.TerminalEnvelopeSha256.Span))
            throw new ArgumentException(
                "The creation terminal SHA-256 does not match its envelope.",
                nameof(publication));
    }

    private static void ValidateCreationOperation(ZLinkCreationOperationId operation)
    {
        if (operation.SourceNodeRid.IsEmpty
            || operation.SourceNodeGeneration == 0
            || (operation.OperationIdHigh == 0 && operation.OperationIdLow == 0))
            throw new ArgumentOutOfRangeException(nameof(operation));
    }

    private void RemoveExpiredCreationTerminals(DateTimeOffset now)
    {
        foreach (var operation in _creationTerminals
                     .Where(pair => pair.Value.ExpiresAt <= now)
                     .Select(static pair => pair.Key)
                     .ToArray())
            _creationTerminals.Remove(operation);
    }

    private static void ValidateAggregateRequest(
        ZLinkAggregatePrepareRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.AggregateId == Guid.Empty
            || request.AggregateGeneration is 0 or > long.MaxValue
            || request.Participants.Count < 1
            || request.InventoryDigest.Length != 32
            || request.TargetDescriptorLifecycleGeneration == 0
            || !IsAggregateCapacityValid(request)
            || request.TargetOwner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(request));
        if (request.Participants.Select(static value => value.Key.Value)
            .Distinct(StringComparer.Ordinal).Count() != request.Participants.Count)
            throw new ArgumentException(
                "Aggregate participant keys must be unique.",
                nameof(request));
        if (!request.Participants.Select(static value => value.Key.Value)
                .SequenceEqual(
                    request.Participants.Select(static value => value.Key.Value)
                        .OrderBy(static value => value, StringComparer.Ordinal),
                    StringComparer.Ordinal))
            throw new ArgumentException(
                "Aggregate participant keys must be canonically sorted.",
                nameof(request));
    }

    private static bool IsAggregateCapacityValid(
        ZLinkAggregatePrepareRequest request)
    {
        var preservesOwner = request.Participants.All(static participant =>
            participant.OwnerTransition
            == ZLinkAuthorityGenerationTransition.Preserve);
        var hasNoCapacityDelta = request.Capacity.Actors == 0
                                 && request.Capacity.Spots == 0
                                 && request.Capacity.SpotType is null;
        return preservesOwner
            ? hasNoCapacityDelta
              && request.Participants.All(static participant =>
                  participant.MembershipMutation.IsEmpty)
            : !hasNoCapacityDelta && IsCapacityVectorValid(request.Capacity);
    }

    private static bool CanIncrement(long value) => value < long.MaxValue;

    private static long Next(ref long value) => checked(++value);

    private sealed record AuthorityScan(
        IReadOnlyList<ZLinkAuthorityEntry> Items,
        DateTimeOffset ExpiresAt);

    private sealed class ReservationState(
        ZLinkObjectReservation reservation,
        ReservationStatus status)
    {
        internal ZLinkObjectReservation Reservation { get; } = reservation;
        internal ReservationStatus Status { get; set; } = status;
        internal ZLinkAuthoritySnapshot? Snapshot { get; set; }
        internal ZLinkCreationTerminalRecord? Terminal { get; set; }
    }

    private sealed class AggregateState(
        ZLinkAggregatePrepareRequest request,
        AggregateStatus status,
        IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
            targetAuthorityOwnerGenerations)
    {
        internal ZLinkAggregatePrepareRequest Request { get; } = request;
        internal AggregateStatus Status { get; set; } = status;
        internal IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
            TargetAuthorityOwnerGenerations { get; } =
                targetAuthorityOwnerGenerations;
    }

    private sealed class RelocationCapacityState(
        ZLinkRelocationCapacityReservationRequest request,
        RelocationCapacityStatus status,
        ulong targetAuthorityOwnerGeneration)
    {
        internal ZLinkRelocationCapacityReservationRequest Request { get; set; } =
            request;
        internal RelocationCapacityStatus Status { get; set; } = status;
        internal ulong TargetAuthorityOwnerGeneration { get; } =
            targetAuthorityOwnerGeneration;
    }

    private readonly record struct PlacementCapacityKey(
        ZLinkMeshNodeDescriptorKey Descriptor,
        ulong DescriptorLifecycleGeneration,
        ZLinkPlacementObjectKind ObjectKind,
        string StableType)
    {
        internal static PlacementCapacityKey From(
            ZLinkPlacementAllocation allocation) =>
            new(
                allocation.Descriptor,
                allocation.DescriptorLifecycleGeneration,
                allocation.ObjectKind,
                allocation.StableType);

    }

    private enum RelocationCapacityStatus
    {
        Reserved,
        Prepared,
        Committed,
        Aborted
    }

    private enum ReservationStatus
    {
        Reserved,
        Created,
        Rejected,
        Failed,
        Aborted
    }

    private enum AggregateStatus
    {
        Prepared,
        Committed,
        Aborted
    }
}
