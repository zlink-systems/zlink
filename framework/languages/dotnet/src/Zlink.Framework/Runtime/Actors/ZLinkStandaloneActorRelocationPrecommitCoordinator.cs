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
    IZLinkLocationRepository store
)
{
    internal const string DirectTransferReference = "pending";
    private const int MaxConflictRetries = 8;

    internal static bool IsDirectTransferReference(string reference, uint checksumCrc32c) =>
        checksumCrc32c == 0 && StringComparer.Ordinal.Equals(reference, DirectTransferReference);

    internal async ValueTask<ZLinkAuthoritySnapshot> BeginPreparingAsync(
        ZLinkAuthoritySnapshot source,
        ZLinkActorAuthorityPayload sourceAuthority,
        Guid relocationId,
        long applicationVersion,
        CancellationToken cancellationToken
    )
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
            applicationVersion
        )
        {
            CoordinatorExpectedAuthorityStoreVersion = source.StoreVersion,
            RelocationReference = DirectTransferReference,
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
            source.Payload.Span,
            state,
            root: null
        );
        return await StoreAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(sourceAuthority.ActorId),
                source,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                ),
                current =>
                    Matches(current, relocationId, phase: 1, source, target: null, attempt: 0),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> CaptureAsync(
        ZLinkAuthoritySnapshot preparing,
        ZLinkRelocationEnvelope root,
        CancellationToken cancellationToken
    )
    {
        var projection = RequirePhase(preparing, root.AggregateId, 1);
        var state = projection.State with
        {
            Phase = 2,
            AggregateGeneration = root.AggregateGeneration,
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
            preparing.Payload.Span,
            state,
            root
        );
        return await StoreAsync(
                root.Participants.Single().AuthorityKey,
                preparing,
                new ZLinkAuthorityMutation.Put(
                    payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                ),
                current =>
                    Matches(
                        current,
                        root.AggregateId,
                        phase: 2,
                        preparing,
                        target: null,
                        attempt: 0
                    ),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    internal ValueTask<ZLinkAuthoritySnapshot> CommitTargetAsync(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope root,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkActorAuthorityPayload targetAuthority,
        CancellationToken cancellationToken
    ) =>
        CommitTargetAsync(
            captured,
            root,
            prepare,
            targetAuthority,
            checked(captured.AuthorityOwnerGeneration + 1),
            static () => true,
            TimeSpan.Zero,
            cancellationToken
        );

    /// <summary>
    /// Location runtime §10: the target owns its <c>NewOwner</c> CAS. It
    /// resubmits the same mutation with the same expected source StoreVersion
    /// and RelocationId after an uncertain response or an auxiliary-row
    /// conflict while the authority row still holds that source fence and the
    /// target owner lease is valid. This also covers the same-target
    /// resubmission after the source lease expired, because the Store fence is
    /// unchanged. The attempt ends only on a confirmed commit, a changed fence
    /// (source <c>Preserve</c> or another owner), the end of the target lease,
    /// or cancellation.
    /// </summary>
    internal async ValueTask<ZLinkAuthoritySnapshot> CommitTargetAsync(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope root,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong targetAuthorityOwnerGeneration,
        Func<bool> isTargetLeaseValid,
        TimeSpan resubmitInterval,
        CancellationToken cancellationToken
    )
    {
        // A .NET source persists phases 1/2 while it captures its in-memory
        // handoff.  That is a recovery aid, not a service-wire precondition:
        // spec 52 makes the target the sole writer of the owner-changing CAS
        // after CUTOVER.  A foreign source therefore legitimately still has
        // its exact steady authority row here.  Admit that form only when the
        // envelope and PREPARE fence prove it is the same source attempt.
        var projection = RequireTargetCommitPrecondition(captured, root, prepare);
        var targetOwner = new ZLinkLocationOwnerToken(
            prepare.Target.OwnerId,
            checked((long)prepare.Target.OwnerLeaseGeneration)
        );
        if (
            prepare.RelocationId
                != new ZLinkServiceWireCodec.RelocationWireId(
                    projection.RelocationHigh,
                    projection.RelocationLow
                )
            || prepare.TargetAttemptGeneration == 0
            || prepare.Target.NodeRid != targetAuthority.NodeRid
            || prepare.Target.NodeGeneration != targetAuthority.NodeGeneration
            || !StringComparer.Ordinal.Equals(targetOwner.OwnerId, targetAuthority.OwnerId)
            || checked((ulong)targetOwner.LeaseGeneration) != targetAuthority.OwnerLeaseGeneration
            || targetAuthorityOwnerGeneration <= captured.AuthorityOwnerGeneration
        )
            throw DataLost("Target cutover changed its prepared attempt fence.");
        var state = projection.State with
        {
            Phase = (byte)ZLinkStandaloneActorCanonicalPhase.Committed,
            TargetAttemptGeneration = prepare.TargetAttemptGeneration,
            TargetNodeRid = prepare.Target.NodeRid.ToHex(),
            TargetNodeGeneration = prepare.Target.NodeGeneration,
            TargetOwnerId = targetOwner.OwnerId,
            TargetOwnerLeaseGeneration = checked((ulong)targetOwner.LeaseGeneration),
        };
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
            ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority),
            state,
            root
        );
        var key = root.Participants.Single().AuthorityKey;
        var mutation = new ZLinkAuthorityMutation.Put(
            payload,
            ZLinkAuthorityGenerationTransition.NewOwner,
            targetOwner,
            captured.Allocation with
            {
                State = ZLinkPlacementAllocationState.Active,
                Descriptor = new ZLinkMeshNodeDescriptorKey(
                    captured.Allocation.Descriptor.MeshName,
                    prepare.Target.NodeRid
                ),
                DescriptorLifecycleGeneration = prepare.Target.NodeGeneration,
            },
            targetAuthorityOwnerGeneration
        );
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!isTargetLeaseValid())
                throw new ZLinkRelocationTargetSettledException(
                    "Standalone Actor target owner lease ended before its authority CAS was confirmed."
                );
            ZLinkAuthorityCompareExchangeResult? result;
            try
            {
                result = await store
                    .CompareExchangeAuthorityAsync(
                        key,
                        captured.StoreVersion,
                        mutation,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            catch (Exception) when (!cancellationToken.IsCancellationRequested)
            {
                //  §10: an uncertain response is not guessed; the same key and
                //  expected version are read again before any resubmission.
                result = null;
            }
            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            var current = result switch
            {
                ZLinkAuthorityCompareExchangeResult.Conflict conflict => conflict.Current,
                null => await TryReadAfterUncertainAsync(key, cancellationToken)
                    .ConfigureAwait(false),
                _ => throw new InvalidOperationException(
                    "Authority Store rejected the standalone Actor target CAS."
                ),
            };
            if (current is ZLinkAuthorityReadResult.Found found)
            {
                if (
                    MatchesCommitted(
                        found.Snapshot,
                        root.AggregateId,
                        captured,
                        targetOwner,
                        prepare
                    )
                )
                    return found.Snapshot;
                if (
                    !StringComparer.Ordinal.Equals(
                        found.Snapshot.StoreVersion,
                        captured.StoreVersion
                    )
                )
                    throw new ZLinkRelocationTargetSettledException(
                        "Standalone Actor target authority CAS lost its expected source fence."
                    );
            }
            else if (current is ZLinkAuthorityReadResult.Missing)
                throw DataLost("Standalone Actor target CAS lost its source authority.");
            await Task.Delay(resubmitInterval, cancellationToken).ConfigureAwait(false);
        }
    }

    private async ValueTask<ZLinkAuthorityReadResult?> TryReadAfterUncertainAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken
    )
    {
        try
        {
            return await store.ReadAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            //  The Store is still unavailable: the result stays unknown and the
            //  target keeps the same fence while its lease is valid.
            return null;
        }
    }

    /// <summary>
    /// Location runtime §6.1: the source <c>Preserve</c> fence. One CAS on the
    /// StoreVersion the target's NewOwner CAS expects (the captured source
    /// attempt) that keeps the source owner and restores its steady payload.
    /// Returns the preserved record, or <c>null</c> when the record changed
    /// first (the caller reads it again). A Store failure propagates as an
    /// indeterminate result.
    /// </summary>
    internal async ValueTask<ZLinkAuthoritySnapshot?> TryPreserveSourceAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot captured,
        Guid relocationId,
        CancellationToken cancellationToken
    )
    {
        if (
            !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                captured.Payload.Span,
                out var projection
            ) || !SameRelocation(projection, relocationId)
        )
            throw DataLost("Standalone Actor source Preserve lost its captured source attempt.");
        var result = await store
            .CompareExchangeAuthorityAsync(
                key,
                captured.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    projection.SteadyAuthorityPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                ),
                cancellationToken
            )
            .ConfigureAwait(false);
        return result switch
        {
            ZLinkAuthorityCompareExchangeResult.Stored stored => stored.Snapshot,
            ZLinkAuthorityCompareExchangeResult.Conflict => null,
            _ => throw new InvalidOperationException(
                "Authority Store rejected the standalone Actor source Preserve."
            ),
        };
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> AbortSourceAsync(
        ZLinkAuthorityKey key,
        Guid relocationId,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            var read = await store.ReadAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    "Standalone Actor source authority disappeared during precommit abort."
                );
            if (
                !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var projection
                )
            )
                return found.Snapshot;
            if (
                !SameRelocation(projection, relocationId)
                || projection.Phase is not (1 or 2 or 9)
                || !StringComparer.Ordinal.Equals(
                    found.Snapshot.OwnerId,
                    projection.State.SourceOwnerId
                )
                || found.Snapshot.OwnerLeaseGeneration
                    != checked((long)projection.State.SourceOwnerLeaseGeneration)
            )
                throw DataLost("Standalone Actor precommit abort lost its exact source fence.");
            var result = await store
                .CompareExchangeAuthorityAsync(
                    key,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        projection.SteadyAuthorityPayload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Conflict)
            {
                var afterConflict = await store
                    .ReadAuthorityAsync(key, cancellationToken)
                    .ConfigureAwait(false);
                if (
                    afterConflict is not ZLinkAuthorityReadResult.Found current
                    || !StringComparer.Ordinal.Equals(
                        current.Snapshot.StoreVersion,
                        found.Snapshot.StoreVersion
                    )
                )
                    continue;
                // The source row is unchanged. Recovery can use its exact
                // version and owner identity through Restore.
                result = await store
                    .CompareExchangeAuthorityAsync(
                        key,
                        found.Snapshot.StoreVersion,
                        new ZLinkAuthorityMutation.Restore(
                            projection.SteadyAuthorityPayload,
                            new ZLinkLocationOwnerToken(
                                found.Snapshot.OwnerId,
                                found.Snapshot.OwnerLeaseGeneration
                            )
                        ),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            if (result is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected standalone Actor precommit abort."
                );
        }
    }

    /// <summary>
    /// Removes an abandoned Preparing marker without racing a source that has
    /// already published Captured. A later phase, another relocation, or a
    /// steady row is left unchanged.
    /// </summary>
    internal async ValueTask<ZLinkAuthoritySnapshot> AbortPreparingAsync(
        ZLinkAuthorityKey key,
        Guid relocationId,
        CancellationToken cancellationToken
    )
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var read = await store.ReadAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    "Standalone Actor Preparing authority disappeared during startup recovery."
                );
            if (
                !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var projection
                )
                || !SameRelocation(projection, relocationId)
                || projection.Phase != 1
            )
                return found.Snapshot;
            if (
                !StringComparer.Ordinal.Equals(
                    found.Snapshot.OwnerId,
                    projection.State.SourceOwnerId
                )
                || found.Snapshot.OwnerLeaseGeneration
                    != checked((long)projection.State.SourceOwnerLeaseGeneration)
            )
                throw DataLost("Standalone Actor Preparing abort lost its exact source fence.");
            var result = await store
                .CompareExchangeAuthorityAsync(
                    key,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        projection.SteadyAuthorityPayload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Conflict)
                result = await store
                    .CompareExchangeAuthorityAsync(
                        key,
                        found.Snapshot.StoreVersion,
                        new ZLinkAuthorityMutation.Restore(
                            projection.SteadyAuthorityPayload,
                            new ZLinkLocationOwnerToken(
                                found.Snapshot.OwnerId,
                                found.Snapshot.OwnerLeaseGeneration
                            )
                        ),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            if (result is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected standalone Actor Preparing abort."
                );
        }
        throw Moving("Preparing abort conflicted after the bounded retry limit");
    }

    internal static bool IsSourcePrecommit(ZLinkAuthoritySnapshot snapshot, Guid relocationId)
    {
        if (
            !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out var projection
            )
            || !SameRelocation(projection, relocationId)
            || projection.Phase is not (1 or 2 or 9)
        )
            return false;
        return StringComparer.Ordinal.Equals(snapshot.OwnerId, projection.State.SourceOwnerId)
            && snapshot.OwnerLeaseGeneration
                == checked((long)projection.State.SourceOwnerLeaseGeneration);
    }

    private async ValueTask<ZLinkAuthoritySnapshot> StoreAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        ZLinkAuthorityMutation.Put mutation,
        Func<ZLinkAuthoritySnapshot, bool> reconcile,
        CancellationToken cancellationToken
    )
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            ZLinkAuthorityCompareExchangeResult result;
            try
            {
                result = await store
                    .CompareExchangeAuthorityAsync(
                        key,
                        expected.StoreVersion,
                        mutation,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch
            {
                var readBack = await store
                    .ReadAuthorityAsync(key, CancellationToken.None)
                    .ConfigureAwait(false);
                if (readBack is ZLinkAuthorityReadResult.Found found && reconcile(found.Snapshot))
                    return found.Snapshot;
                throw;
            }

            if (result is ZLinkAuthorityCompareExchangeResult.Stored stored)
                return stored.Snapshot;
            if (
                result
                    is ZLinkAuthorityCompareExchangeResult.Conflict
                    {
                        Current: ZLinkAuthorityReadResult.Found current
                    }
                && reconcile(current.Snapshot)
            )
                return current.Snapshot;
            // Provider CAS also fences auxiliary live-owner/capacity rows.
            // Their version can change while the authority row remains the
            // exact Store-confirmed value we expected. That conflict did not
            // select another relocation or owner, so retry this same local
            // precommit transition. A changed authority StoreVersion still
            // leaves this path immediately and is never overwritten.
            if (
                result
                    is ZLinkAuthorityCompareExchangeResult.Conflict
                    {
                        Current: ZLinkAuthorityReadResult.Found unchanged
                    }
                && StringComparer.Ordinal.Equals(
                    unchanged.Snapshot.StoreVersion,
                    expected.StoreVersion
                )
            )
                continue;
            break;
        }
        throw Moving("precommit authority compare-exchange conflicted");
    }

    private static ZLinkCanonicalRelocationAuthorityProjection RequirePhase(
        ZLinkAuthoritySnapshot snapshot,
        Guid relocationId,
        byte phase
    )
    {
        if (
            !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out var projection
            )
            || !SameRelocation(projection, relocationId)
            || projection.Phase != phase
        )
            throw DataLost($"Standalone Actor relocation is not in durable phase '{phase}'.");
        return projection;
    }

    /// <summary>
    /// Location runtime §10: whether <paramref name="current"/> still is the
    /// source fence the target's NewOwner CAS expects — the captured source
    /// attempt of this relocation, or the exact Command 40 StoreVersion of a
    /// foreign steady source.
    /// </summary>
    internal static bool HoldsTargetCommitFence(
        ZLinkAuthoritySnapshot current,
        ZLinkRelocationEnvelope root,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare
    ) =>
        ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(current.Payload.Span, out var canonical)
            ? SameRelocation(canonical, root.AggregateId) && canonical.Phase == 2
            : StringComparer.Ordinal.Equals(
                current.StoreVersion,
                prepare.Coordinator.ExpectedAuthorityStoreVersion
            );

    private static ZLinkCanonicalRelocationAuthorityProjection RequireTargetCommitPrecondition(
        ZLinkAuthoritySnapshot captured,
        ZLinkRelocationEnvelope root,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare
    )
    {
        //  Location runtime §10: a record that no longer is the expected source
        //  fence (for example the source's Preserve) settles the attempt
        //  against this target even when its owner matches.
        if (!HoldsTargetCommitFence(captured, root, prepare))
            throw new ZLinkRelocationTargetSettledException(
                "Standalone Actor target cutover no longer holds its expected source fence."
            );
        if (
            ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                captured.Payload.Span,
                out var canonical
            )
        )
            return canonical;

        var participant = root.Participants.Single();
        if (
            participant.AuthorityKey
                != ZLinkActorAuthorityPayloadCodec.AuthorityKey(prepare.Object.ObjectId)
            || participant.ObjectGeneration != captured.ObjectGeneration
            || participant.AuthorityOwnerGeneration != captured.AuthorityOwnerGeneration
            || prepare.Object.ObjectGeneration != captured.ObjectGeneration
            || prepare.Object.ExpectedAuthorityOwnerGeneration != captured.AuthorityOwnerGeneration
            || !StringComparer.Ordinal.Equals(captured.OwnerId, prepare.Coordinator.OwnerId)
            || captured.OwnerLeaseGeneration != checked((long)prepare.Coordinator.LeaseGeneration)
            || captured.Allocation.Descriptor.Rid != prepare.Coordinator.NodeRid
            || captured.Allocation.DescriptorLifecycleGeneration
                != prepare.Coordinator.NodeGeneration
            || prepare.SourceNodeRid != prepare.Coordinator.NodeRid
            || prepare.SourceNodeGeneration != prepare.Coordinator.NodeGeneration
        )
            throw DataLost("Standalone Actor target cutover changed its foreign source fence.");

        var (high, low) = RelocationParts(root.AggregateId);
        if (prepare.RelocationId.High != high || prepare.RelocationId.Low != low)
            throw DataLost(
                "Standalone Actor target cutover changed its foreign relocation identity."
            );
        var state = new ZLinkCanonicalRelocationAuthorityState(
            high,
            low,
            0,
            prepare.SourceNodeRid.ToHex(),
            prepare.SourceNodeGeneration,
            captured.OwnerId,
            checked((ulong)captured.OwnerLeaseGeneration),
            string.Empty,
            0,
            string.Empty,
            0,
            prepare.Coordinator.OwnerId,
            prepare.Coordinator.LeaseGeneration,
            prepare.Coordinator.NodeRid.ToHex(),
            prepare.Coordinator.NodeGeneration,
            2,
            checked((long)prepare.ApplicationVersion)
        )
        {
            AggregateGeneration = root.AggregateGeneration,
            CoordinatorExpectedAuthorityStoreVersion = prepare
                .Coordinator
                .ExpectedAuthorityStoreVersion,
            RelocationReference = DirectTransferReference,
        };
        return new ZLinkCanonicalRelocationAuthorityProjection(
            high,
            low,
            0,
            string.Empty,
            0,
            2,
            string.Empty,
            0,
            checked((long)prepare.ApplicationVersion),
            captured.Payload,
            state
        )
        {
            AggregateGeneration = root.AggregateGeneration,
        };
    }

    private static bool Matches(
        ZLinkAuthoritySnapshot current,
        Guid relocationId,
        byte phase,
        ZLinkAuthoritySnapshot source,
        ZLinkLocationOwnerToken? target,
        ulong attempt
    )
    {
        if (
            !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Payload.Span,
                out var projection
            )
            || !SameRelocation(projection, relocationId)
            || projection.Phase != phase
            || projection.TargetAttemptGeneration != attempt
        )
            return false;
        var expectedOwner =
            target ?? new ZLinkLocationOwnerToken(source.OwnerId, source.OwnerLeaseGeneration);
        return StringComparer.Ordinal.Equals(current.OwnerId, expectedOwner.OwnerId)
            && current.OwnerLeaseGeneration == expectedOwner.LeaseGeneration;
    }

    private static bool MatchesCommitted(
        ZLinkAuthoritySnapshot current,
        Guid relocationId,
        ZLinkAuthoritySnapshot source,
        ZLinkLocationOwnerToken targetOwner,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare
    )
    {
        if (
            !Matches(
                current,
                relocationId,
                phase: (byte)ZLinkStandaloneActorCanonicalPhase.Committed,
                source,
                targetOwner,
                prepare.TargetAttemptGeneration
            )
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                current.Payload.Span,
                out var projection
            )
        )
            return false;
        return StringComparer.Ordinal.Equals(
                projection.State.TargetNodeRid,
                prepare.Target.NodeRid.ToHex()
            )
            && projection.State.TargetNodeGeneration == prepare.Target.NodeGeneration
            && StringComparer.Ordinal.Equals(projection.TargetOwnerId, targetOwner.OwnerId)
            && projection.TargetOwnerLeaseGeneration == checked((ulong)targetOwner.LeaseGeneration);
    }

    private static bool SameRelocation(
        ZLinkCanonicalRelocationAuthorityProjection projection,
        Guid relocationId
    )
    {
        var (high, low) = RelocationParts(relocationId);
        return projection.RelocationHigh == high && projection.RelocationLow == low;
    }

    private static (ulong High, ulong Low) RelocationParts(Guid relocationId)
    {
        if (relocationId == Guid.Empty)
            throw new ArgumentOutOfRangeException(nameof(relocationId));
        Span<byte> bytes = stackalloc byte[16];
        relocationId.TryWriteBytes(bytes, bigEndian: true, out _);
        return (
            BinaryPrimitives.ReadUInt64BigEndian(bytes),
            BinaryPrimitives.ReadUInt64BigEndian(bytes[8..])
        );
    }

    private static ZLinkRelocationDataLostException DataLost(string message) => new(message);

    private static ZLinkFrameworkException Moving(string message) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Standalone Actor relocation {message}.",
            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff
        );
}
