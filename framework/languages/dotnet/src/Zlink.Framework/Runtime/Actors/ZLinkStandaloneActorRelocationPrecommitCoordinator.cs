using System.Buffers.Binary;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Persists the standalone Actor source capture and the target-owned cutover
/// CAS. Target preparation never reserves Location Store capacity or changes
/// the source-owned authority row.
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
            source.OwnerId,
            checked((ulong)source.OwnerLeaseGeneration),
            sourceAuthority.NodeRid.ToHex(),
            sourceAuthority.NodeGeneration,
            Phase: 1,
            applicationVersion)
        {
            CoordinatorExpectedAuthorityStoreVersion = source.StoreVersion
        };
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
                    attempt: 0),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> CaptureAsync(
        ZLinkAuthoritySnapshot preparing,
        ZLinkRelocationEnvelope root,
        CancellationToken cancellationToken)
    {
        var projection = RequirePhase(preparing, root.AggregateId, 1);
        var state = projection.State with
        {
            Phase = 2
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
                    attempt: 0),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal ValueTask<ZLinkAuthoritySnapshot> CommitTargetAsync(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope root,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkActorAuthorityPayload targetAuthority,
        CancellationToken cancellationToken) =>
        CommitTargetAsync(
            captured,
            root,
            prepare,
            targetAuthority,
            checked(captured.AuthorityOwnerGeneration + 1),
            cancellationToken);

    internal async ValueTask<ZLinkAuthoritySnapshot> CommitTargetAsync(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope root,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong targetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        // A .NET source persists phases 1/2 while it captures its in-memory
        // handoff.  That is a recovery aid, not a service-wire precondition:
        // spec 52 makes the target the sole writer of the owner-changing CAS
        // after CUTOVER.  A foreign source therefore legitimately still has
        // its exact steady authority row here.  Admit that form only when the
        // envelope and PREPARE fence prove it is the same source attempt.
        var projection = RequireTargetCommitPrecondition(
            captured, root, prepare);
        var targetOwner = new ZLinkLocationOwnerToken(
            prepare.Target.OwnerId,
            checked((long)prepare.Target.OwnerLeaseGeneration));
        if (prepare.RelocationId != new ZLinkServiceWireCodec.RelocationWireId(
                projection.RelocationHigh,
                projection.RelocationLow)
            || prepare.TargetAttemptGeneration == 0
            || prepare.Target.NodeRid != targetAuthority.NodeRid
            || prepare.Target.NodeGeneration != targetAuthority.NodeGeneration
            || !StringComparer.Ordinal.Equals(
                targetOwner.OwnerId,
                targetAuthority.OwnerId)
            || checked((ulong)targetOwner.LeaseGeneration)
               != targetAuthority.OwnerLeaseGeneration
            || targetAuthorityOwnerGeneration
               <= captured.AuthorityOwnerGeneration)
            throw DataLost("Target cutover changed its prepared attempt fence.");
        var state = projection.State with
        {
            Phase = (byte)ZLinkStandaloneActorCanonicalPhase.Committed,
            TargetAttemptGeneration = prepare.TargetAttemptGeneration,
            TargetNodeRid = prepare.Target.NodeRid.ToHex(),
            TargetNodeGeneration = prepare.Target.NodeGeneration,
            TargetOwnerId = targetOwner.OwnerId,
            TargetOwnerLeaseGeneration = checked((ulong)
                targetOwner.LeaseGeneration)
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority),
                state,
                root);
        return await StoreAsync(
                root.Participants.Single().AuthorityKey,
                captured,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    captured.Allocation with
                    {
                        State = ZLinkPlacementAllocationState.Active,
                        Descriptor = new ZLinkMeshNodeDescriptorKey(
                            captured.Allocation.Descriptor.MeshName,
                            prepare.Target.NodeRid),
                        DescriptorLifecycleGeneration =
                            prepare.Target.NodeGeneration
                    },
                    targetAuthorityOwnerGeneration),
                current => MatchesCommitted(
                    current,
                    root.AggregateId,
                    captured,
                    targetOwner,
                    prepare),
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
                || projection.Phase is not (1 or 2 or 9)
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
            // A dead source cannot renew its owner lease, so the plain Put is
            // rejected once recovery runs the abort on another node. Restore
            // is fenced by the exact recorded owner identity and the same
            // StoreVersion CAS instead of lease liveness.
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
            || projection.Phase is not (1 or 2 or 9))
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

    private static ZLinkCanonicalRelocationAuthorityProjection
        RequireTargetCommitPrecondition(
            ZLinkAuthoritySnapshot captured,
            ZLinkRelocationEnvelope root,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                captured.Payload.Span, out var canonical))
        {
            if (SameRelocation(canonical, root.AggregateId)
                && canonical.Phase == 2)
                return canonical;
            throw DataLost(
                "Standalone Actor target cutover changed its durable source attempt.");
        }

        var participant = root.Participants.Single();
        if (participant.AuthorityKey != ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                prepare.Object.ObjectId)
            || participant.ObjectGeneration != captured.ObjectGeneration
            || participant.AuthorityOwnerGeneration
               != captured.AuthorityOwnerGeneration
            || prepare.Object.ObjectGeneration != captured.ObjectGeneration
            || prepare.Object.ExpectedAuthorityOwnerGeneration
               != captured.AuthorityOwnerGeneration
            || !StringComparer.Ordinal.Equals(
                captured.OwnerId, prepare.Coordinator.OwnerId)
            || captured.OwnerLeaseGeneration
               != checked((long)prepare.Coordinator.LeaseGeneration)
            || captured.Allocation.Descriptor.Rid != prepare.Coordinator.NodeRid
            || captured.Allocation.DescriptorLifecycleGeneration
               != prepare.Coordinator.NodeGeneration
            || prepare.SourceNodeRid != prepare.Coordinator.NodeRid
            || prepare.SourceNodeGeneration != prepare.Coordinator.NodeGeneration)
            throw DataLost(
                "Standalone Actor target cutover changed its foreign source fence.");

        var (high, low) = RelocationParts(root.AggregateId);
        if (prepare.RelocationId.High != high || prepare.RelocationId.Low != low)
            throw DataLost(
                "Standalone Actor target cutover changed its foreign relocation identity.");
        var state = new ZLinkCanonicalRelocationAuthorityState(
            high, low, 0,
            prepare.SourceNodeRid.ToHex(), prepare.SourceNodeGeneration,
            captured.OwnerId, checked((ulong)captured.OwnerLeaseGeneration),
            string.Empty, 0, string.Empty, 0,
            prepare.Coordinator.OwnerId, prepare.Coordinator.LeaseGeneration,
            prepare.Coordinator.NodeRid.ToHex(), prepare.Coordinator.NodeGeneration,
            2, checked((long)prepare.ApplicationVersion))
        {
            AggregateGeneration = root.AggregateGeneration,
            CoordinatorExpectedAuthorityStoreVersion = prepare.Coordinator
                .ExpectedAuthorityStoreVersion
        };
        return new ZLinkCanonicalRelocationAuthorityProjection(
            high, low, 0, string.Empty, 0, 2,
            string.Empty, 0, checked((long)prepare.ApplicationVersion),
            captured.Payload, state)
        {
            AggregateGeneration = root.AggregateGeneration
        };
    }

    private static bool Matches(
        ZLinkAuthoritySnapshot current,
        Guid relocationId,
        byte phase,
        ZLinkAuthoritySnapshot source,
        ZLinkLocationOwnerToken? target,
        ulong attempt)
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Payload.Span,
                out var projection)
            || !SameRelocation(projection, relocationId)
            || projection.Phase != phase
            || projection.TargetAttemptGeneration != attempt)
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

    private static bool MatchesCommitted(
        ZLinkAuthoritySnapshot current,
        Guid relocationId,
        ZLinkAuthoritySnapshot source,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        if (!Matches(
                current,
                relocationId,
                phase: (byte)ZLinkStandaloneActorCanonicalPhase.Committed,
                source,
                targetOwner,
                prepare.TargetAttemptGeneration)
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Payload.Span,
                out var projection))
            return false;
        return StringComparer.Ordinal.Equals(
                   projection.State.TargetNodeRid,
                   prepare.Target.NodeRid.ToHex())
               && projection.State.TargetNodeGeneration
               == prepare.Target.NodeGeneration
               && StringComparer.Ordinal.Equals(
                   projection.TargetOwnerId,
                   targetOwner.OwnerId)
               && projection.TargetOwnerLeaseGeneration
               == checked((ulong)targetOwner.LeaseGeneration);
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
