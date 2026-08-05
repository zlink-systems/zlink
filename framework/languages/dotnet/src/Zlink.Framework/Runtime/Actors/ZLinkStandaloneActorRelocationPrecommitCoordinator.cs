using System.Buffers.Binary;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Persists the standalone Actor precommit phases on the source authority.
/// The Prepared CAS also rebinds its reserved capacity fence, allowing the
/// following NewOwner CAS to consume the same fence without a version gap.
/// </summary>
internal sealed class ZLinkStandaloneActorRelocationPrecommitCoordinator(
    IZLinkLocationRepository store)
{
    private const int MaxConflictRetries = 8;

    internal async ValueTask<ZLinkAuthoritySnapshot> BeginPreparingAsync(
        ZLinkAuthoritySnapshot source,
        ZLinkActorAuthorityPayload sourceAuthority,
        Guid relocationId,
        long applicationVersion,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(sourceAuthority);
        var (high, low) = RelocationParts(relocationId);
        var state = new ZLinkCanonicalRelocationAuthorityState(
            high,
            low,
            TargetAttemptGeneration: 0,
            sourceAuthority.NodeRid.ToHex(),
            sourceAuthority.NodeGeneration,
            source.OwnerId,
            checked((ulong)source.OwnerLeaseGeneration),
            TargetNodeRid: string.Empty,
            TargetNodeGeneration: 0,
            TargetOwnerId: string.Empty,
            TargetOwnerLeaseGeneration: 0,
            ReservationGeneration: 0,
            source.OwnerId,
            checked((ulong)source.OwnerLeaseGeneration),
            sourceAuthority.NodeRid.ToHex(),
            sourceAuthority.NodeGeneration,
            Phase: 1,
            RelocationReference: string.Empty,
            RelocationChecksumCrc32c: 0,
            applicationVersion,
            SourceCleanupState: 0);
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(source.Payload.Span, state, root: null);
        return await StoreAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    sourceAuthority.ActorId),
                source,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                current => Matches(
                    current,
                    relocationId,
                    phase: 1,
                    source,
                    target: null,
                    attempt: 0,
                    reference: null),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> CaptureAsync(
        ZLinkAuthoritySnapshot preparing,
        ZLinkRelocationEnvelope root,
        ZLinkRelocationStored stored,
        CancellationToken cancellationToken)
    {
        var projection = RequirePhase(preparing, root.AggregateId, 1);
        var state = projection.State with
        {
            Phase = 2,
            RelocationReference = stored.Reference,
            RelocationChecksumCrc32c = stored.ChecksumCrc32c
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(preparing.Payload.Span, state, root);
        return await StoreAsync(
                root.Participants.Single().AuthorityKey,
                preparing,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                current => Matches(
                    current,
                    root.AggregateId,
                    phase: 2,
                    preparing,
                    target: null,
                    attempt: 0,
                    stored.Reference),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> PrepareTargetAsync(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope root,
        ZLinkRelocationCapacityFence capacityFence,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ulong reservationGeneration,
        CancellationToken cancellationToken)
    {
        if (reservationGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(reservationGeneration));
        var projection = RequirePhase(captured, root.AggregateId, 2);
        var targetOwner = new ZLinkLocationOwnerToken(
            prepare.Candidate.OwnerId,
            checked((long)prepare.Candidate.OwnerLeaseGeneration));
        var state = projection.State with
        {
            Phase = 3,
            TargetAttemptGeneration = prepare.TargetAttemptGeneration,
            TargetNodeRid = prepare.Candidate.NodeRid.ToHex(),
            TargetNodeGeneration = prepare.Candidate.NodeGeneration,
            TargetOwnerId = targetOwner.OwnerId,
            TargetOwnerLeaseGeneration = checked((ulong)
                targetOwner.LeaseGeneration),
            ReservationGeneration = reservationGeneration
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(captured.Payload.Span, state, root);
        return await StoreAsync(
                root.Participants.Single().AuthorityKey,
                captured,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    capacityFence),
                current => MatchesPrepared(
                    current,
                    root.AggregateId,
                    captured,
                    targetOwner,
                    prepare,
                    reservationGeneration,
                    projection.RelocationReference),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> RefreshCapturedRootAsync(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope finalRoot,
        ZLinkRelocationStored storedFinalRoot,
        ZLinkRelocationCapacityFence capacityFence,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(captured);
        ArgumentNullException.ThrowIfNull(finalRoot);
        ArgumentNullException.ThrowIfNull(storedFinalRoot);
        var projection = RequirePhase(captured, finalRoot.AggregateId, 2);
        var state = projection.State with
        {
            RelocationReference = storedFinalRoot.Reference,
            RelocationChecksumCrc32c = storedFinalRoot.ChecksumCrc32c
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(captured.Payload.Span, state, finalRoot);
        return await StoreAsync(
                finalRoot.Participants.Single().AuthorityKey,
                captured,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    capacityFence),
                current => Matches(
                    current,
                    finalRoot.AggregateId,
                    phase: 2,
                    captured,
                    target: null,
                    attempt: 0,
                    storedFinalRoot.Reference),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> CommitTargetAsync(
        ZLinkAuthoritySnapshot prepared,
        ZLinkRelocationEnvelope root,
        ZLinkRelocationCapacityFence capacityFence,
        ZLinkActorAuthorityPayload targetAuthority,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        var projection = RequirePhase(prepared, root.AggregateId, 3);
        if (!StringComparer.Ordinal.Equals(
                projection.TargetOwnerId,
                targetOwner.OwnerId)
            || projection.TargetOwnerLeaseGeneration
               != checked((ulong)targetOwner.LeaseGeneration)
            || !StringComparer.Ordinal.Equals(
                projection.State.TargetNodeRid,
                targetAuthority.NodeRid.ToHex())
            || projection.State.TargetNodeGeneration
               != targetAuthority.NodeGeneration)
            throw DataLost("Prepared target fence changed before owner commit.");
        var state = projection.State with
        {
            Phase = (byte)ZLinkStandaloneActorCanonicalPhase.Committed
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority),
                state,
                root);
        return await StoreAsync(
                root.Participants.Single().AuthorityKey,
                prepared,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    capacityFence),
                current => Matches(
                    current,
                    root.AggregateId,
                    phase: 4,
                    prepared,
                    targetOwner,
                    projection.TargetAttemptGeneration,
                    projection.RelocationReference),
                cancellationToken,
                retryAuxiliaryConflicts: true)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> AbortSourceAsync(
        ZLinkAuthorityKey key,
        Guid relocationId,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var read = await store.ReadAuthorityAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    "Standalone Actor source authority disappeared during precommit abort.");
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var projection))
                return found.Snapshot;
            if (!SameRelocation(projection, relocationId)
                || projection.Phase is not (1 or 2 or 3 or 9)
                || !StringComparer.Ordinal.Equals(
                    found.Snapshot.OwnerId,
                    projection.State.SourceOwnerId)
                || found.Snapshot.OwnerLeaseGeneration
                   != checked((long)projection.State.SourceOwnerLeaseGeneration))
                throw DataLost(
                    "Standalone Actor precommit abort lost its exact source fence.");
            var result = await store.CompareExchangeAuthorityAsync(
                    key,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        projection.SteadyAuthorityPayload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null),
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            if (result is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected standalone Actor precommit abort.");
        }
        throw Moving("precommit abort conflicted after the bounded retry limit");
    }

    /// <summary>
    /// Removes an abandoned Preparing marker without racing a source that has
    /// already published Captured. A later phase, another relocation, or a
    /// steady row is left unchanged.
    /// </summary>
    internal async ValueTask<ZLinkAuthoritySnapshot> AbortPreparingAsync(
        ZLinkAuthorityKey key,
        Guid relocationId,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var read = await store.ReadAuthorityAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    "Standalone Actor Preparing authority disappeared during startup recovery.");
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var projection)
                || !SameRelocation(projection, relocationId)
                || projection.Phase != 1)
                return found.Snapshot;
            if (!StringComparer.Ordinal.Equals(
                    found.Snapshot.OwnerId,
                    projection.State.SourceOwnerId)
                || found.Snapshot.OwnerLeaseGeneration
                   != checked((long)projection.State.SourceOwnerLeaseGeneration))
                throw DataLost(
                    "Standalone Actor Preparing abort lost its exact source fence.");
            var result = await store.CompareExchangeAuthorityAsync(
                    key,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        projection.SteadyAuthorityPayload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null),
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Conflict)
                result = await store.CompareExchangeAuthorityAsync(
                        key,
                        found.Snapshot.StoreVersion,
                        new ZLinkAuthorityMutation.Restore(
                            projection.SteadyAuthorityPayload,
                            new ZLinkLocationOwnerToken(
                                found.Snapshot.OwnerId,
                                found.Snapshot.OwnerLeaseGeneration)),
                        cancellationToken)
                    .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            if (result is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected standalone Actor Preparing abort.");
        }
        throw Moving(
            "Preparing abort conflicted after the bounded retry limit");
    }

    internal static bool IsSourcePrecommit(
        ZLinkAuthoritySnapshot snapshot,
        Guid relocationId)
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out var projection)
            || !SameRelocation(projection, relocationId)
            || projection.Phase is not (1 or 2 or 3 or 9))
            return false;
        return StringComparer.Ordinal.Equals(
                   snapshot.OwnerId,
                   projection.State.SourceOwnerId)
               && snapshot.OwnerLeaseGeneration
               == checked((long)projection.State.SourceOwnerLeaseGeneration);
    }

    private async ValueTask<ZLinkAuthoritySnapshot> StoreAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        ZLinkAuthorityMutation.Put mutation,
        Func<ZLinkAuthoritySnapshot, bool> reconcile,
        CancellationToken cancellationToken,
        bool retryAuxiliaryConflicts = false)
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            ZLinkAuthorityCompareExchangeResult result;
            try
            {
                result = await store.CompareExchangeAuthorityAsync(
                        key,
                        expected.StoreVersion,
                        mutation,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch
            {
                var readBack = await store.ReadAuthorityAsync(
                        key,
                        CancellationToken.None)
                    .ConfigureAwait(false);
                if (readBack is ZLinkAuthorityReadResult.Found found
                    && reconcile(found.Snapshot))
                    return found.Snapshot;
                throw;
            }

            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            if (result is ZLinkAuthorityCompareExchangeResult.Conflict
                {
                    Current: ZLinkAuthorityReadResult.Found current
                }
                && reconcile(current.Snapshot))
                return current.Snapshot;
            if (retryAuxiliaryConflicts
                && result is ZLinkAuthorityCompareExchangeResult.Conflict
                {
                    Current: ZLinkAuthorityReadResult.Found unchanged
                }
                && StringComparer.Ordinal.Equals(
                    unchanged.Snapshot.StoreVersion,
                    expected.StoreVersion))
                continue;
            break;
        }
        throw Moving("precommit authority compare-exchange conflicted");
    }

    private static ZLinkCanonicalRelocationAuthorityProjection RequirePhase(
        ZLinkAuthoritySnapshot snapshot,
        Guid relocationId,
        byte phase)
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out var projection)
            || !SameRelocation(projection, relocationId)
            || projection.Phase != phase)
            throw DataLost(
                $"Standalone Actor relocation is not in durable phase '{phase}'.");
        return projection;
    }

    private static bool Matches(
        ZLinkAuthoritySnapshot current,
        Guid relocationId,
        byte phase,
        ZLinkAuthoritySnapshot source,
        ZLinkLocationOwnerToken? target,
        ulong attempt,
        string? reference)
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Payload.Span,
                out var projection)
            || !SameRelocation(projection, relocationId)
            || projection.Phase != phase
            || projection.TargetAttemptGeneration != attempt
            || !StringComparer.Ordinal.Equals(
                projection.RelocationReference,
                reference ?? string.Empty))
            return false;
        var expectedOwner = target ?? new ZLinkLocationOwnerToken(
            source.OwnerId,
            source.OwnerLeaseGeneration);
        return StringComparer.Ordinal.Equals(
                   current.OwnerId,
                   expectedOwner.OwnerId)
               && current.OwnerLeaseGeneration
               == expectedOwner.LeaseGeneration;
    }

    private static bool MatchesPrepared(
        ZLinkAuthoritySnapshot current,
        Guid relocationId,
        ZLinkAuthoritySnapshot source,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ulong reservationGeneration,
        string reference)
    {
        if (!Matches(
                current,
                relocationId,
                phase: 3,
                source,
                target: null,
                prepare.TargetAttemptGeneration,
                reference)
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Payload.Span,
                out var projection))
            return false;
        return StringComparer.Ordinal.Equals(
                   projection.State.TargetNodeRid,
                   prepare.Candidate.NodeRid.ToHex())
               && projection.State.TargetNodeGeneration
               == prepare.Candidate.NodeGeneration
               && StringComparer.Ordinal.Equals(
                   projection.TargetOwnerId,
                   targetOwner.OwnerId)
               && projection.TargetOwnerLeaseGeneration
               == checked((ulong)targetOwner.LeaseGeneration)
               && projection.State.ReservationGeneration
               == reservationGeneration;
    }

    private static bool SameRelocation(
        ZLinkCanonicalRelocationAuthorityProjection projection,
        Guid relocationId)
    {
        var (high, low) = RelocationParts(relocationId);
        return projection.RelocationHigh == high
               && projection.RelocationLow == low;
    }

    private static (ulong High, ulong Low) RelocationParts(Guid relocationId)
    {
        if (relocationId == Guid.Empty)
            throw new ArgumentOutOfRangeException(nameof(relocationId));
        Span<byte> bytes = stackalloc byte[16];
        relocationId.TryWriteBytes(bytes, bigEndian: true, out _);
        return (
            BinaryPrimitives.ReadUInt64BigEndian(bytes),
            BinaryPrimitives.ReadUInt64BigEndian(bytes[8..]));
    }

    private static ZLinkRelocationDataLostException DataLost(string message) =>
        new(message);

    private static ZLinkFrameworkException Moving(string message) => new(
        ZLinkFrameworkErrorKind.Unavailable,
        $"Standalone Actor relocation {message}.",
        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
}
