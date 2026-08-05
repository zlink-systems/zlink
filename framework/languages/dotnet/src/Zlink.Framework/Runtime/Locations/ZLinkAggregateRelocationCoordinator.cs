using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkAggregateRelocationParticipant(
    ZLinkRelocationParticipantEnvelope Envelope,
    string ExpectedStoreVersion,
    ZLinkAuthorityGenerationTransition OwnerTransition,
    ReadOnlyMemory<byte> ApplicationAuthorityPayload,
    ReadOnlyMemory<byte> MembershipMutation);

internal sealed record ZLinkAggregateRelocationRequest(
    Guid AggregateId,
    ulong AggregateGeneration,
    IReadOnlyList<ZLinkAggregateRelocationParticipant> Participants,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetDescriptorLifecycleGeneration,
    ZLinkCapacityVector Capacity,
    ZLinkLocationOwnerToken TargetOwner,
    ZLinkRelocationEnvelope? CanonicalEnvelope = null);

internal sealed record ZLinkAggregateRelocationPublished(
    ZLinkAggregateFence Fence,
    ZLinkRelocationStored Relocation,
    ZLinkRelocationEnvelope Envelope);

internal sealed record ZLinkPreparedAggregateRelocation(
    ZLinkAggregateFence Fence,
    ZLinkRelocationStored Relocation,
    ZLinkRelocationEnvelope Envelope,
    IReadOnlyList<ZLinkAggregateRelocationParticipant> Participants,
    ReadOnlyMemory<byte> InventoryDigest,
    IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
        TargetAuthorityOwnerGenerations)
{
    internal ulong TargetAuthorityOwnerGeneration(ZLinkAuthorityKey key) =>
        TargetAuthorityOwnerGenerations.TryGetValue(key, out var generation)
        && generation is > 0 and <= long.MaxValue
            ? generation
            : throw new ZLinkRelocationDataLostException(
                $"Prepared relocation has no target authority generation for '{key.Value}'.");
}

internal sealed class ZLinkAuthorityGenerationExhaustedException(string operation)
    : InvalidOperationException(
        $"Authority generation was exhausted while {operation}.");

internal sealed class ZLinkAggregateRelocationCoordinator(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore)
{
    private const int MaxConflictRetries = 8;
    private const int MaxPublicationProbeConcurrency = 64;
    private static readonly TimeSpan Retention = TimeSpan.FromHours(24);
    private static readonly TimeSpan ReconciliationTimeout =
        TimeSpan.FromSeconds(5);
    private readonly object canonicalRootCacheGate = new();
    private string? canonicalRootCacheReference;
    private uint canonicalRootCacheChecksum;
    private ZLinkRelocationEnvelope? canonicalRootCache;

    internal async ValueTask<ZLinkAggregateRelocationPublished> PublishAsync(
        ZLinkAggregateRelocationRequest request,
        CancellationToken cancellationToken = default)
    {
        var prepared = await PrepareAsync(request, cancellationToken)
            .ConfigureAwait(false);
        return await CommitAsync(prepared, cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>
    /// Stores and verifies the immutable root, then reserves the exact
    /// authority aggregate without publishing it. The target must finish
    /// factory and Restore staging before calling <see cref="CommitAsync"/>.
    /// </summary>
    internal async ValueTask<ZLinkPreparedAggregateRelocation> PrepareAsync(
        ZLinkAggregateRelocationRequest request,
        CancellationToken cancellationToken = default) =>
        await PrepareCoreAsync(
                request,
                acceptedRelocation: null,
                cancellationToken)
            .ConfigureAwait(false);

    /// <summary>
    /// Reserves the authority aggregate against an immutable root that the
    /// source already stored and verified. The target must not write a second
    /// root because relocation references are opaque and need not be
    /// content-addressed.
    /// </summary>
    internal async ValueTask<ZLinkPreparedAggregateRelocation>
        PrepareExistingAsync(
            ZLinkAggregateRelocationRequest request,
            ZLinkRelocationStored acceptedRelocation,
            CancellationToken cancellationToken = default) =>
        await PrepareCoreAsync(
                request,
                acceptedRelocation,
                cancellationToken)
            .ConfigureAwait(false);

    private async ValueTask<ZLinkPreparedAggregateRelocation> PrepareCoreAsync(
        ZLinkAggregateRelocationRequest request,
        ZLinkRelocationStored? acceptedRelocation,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        Validate(request);
        cancellationToken.ThrowIfCancellationRequested();

        var inventoryDigest = ZLinkAggregateInventoryDigest.Compute(
            request.Participants);
        var envelope = request.CanonicalEnvelope ?? new ZLinkRelocationEnvelope(
            request.AggregateId,
            request.AggregateGeneration,
            inventoryDigest,
            request.Participants
                .Select(static participant => participant.Envelope)
                .ToArray());
        var ownsStoredRoot = acceptedRelocation is null;
        var stored = acceptedRelocation
            ?? (await ZLinkRelocationTreeStore.PutAsync(
                    relocationStore,
                    envelope,
                    Retention,
                    cancellationToken)
                .ConfigureAwait(false)).Root;
        var fence = new ZLinkAggregateFence(
            request.AggregateId,
            request.AggregateGeneration);
        IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
            targetAuthorityOwnerGenerations =
                new Dictionary<ZLinkAuthorityKey, ulong>();
        try
        {
            var restored = await ZLinkRelocationTreeStore.GetAsync(
                    relocationStore,
                    stored.Reference,
                    stored.ChecksumCrc32c,
                    cancellationToken)
                .ConfigureAwait(false);
            if (restored.AggregateId != envelope.AggregateId
                || (envelope.CanonicalLogicalStream.IsEmpty
                    ? restored.AggregateGeneration != envelope.AggregateGeneration
                    : restored.CanonicalApplicationVersion
                      != envelope.CanonicalApplicationVersion)
                || !restored.InventoryDigest.Span.SequenceEqual(
                    inventoryDigest.AsSpan()))
                throw new ZLinkRelocationDataLostException(
                    "Relocation Store did not preserve the aggregate root.");

            var publicationParticipants = request.Participants
                .Select(participant => new ZLinkAggregateParticipant(
                    participant.Envelope.AuthorityKey,
                    participant.ExpectedStoreVersion,
                    participant.OwnerTransition,
                    envelope.CanonicalLogicalStream.IsEmpty
                        ? ZLinkRelocationAuthorityPayloadCodec.Encode(
                        new ZLinkRelocationAuthorityPayload(
                            stored.Reference,
                            stored.ChecksumCrc32c,
                            request.AggregateId,
                            request.AggregateGeneration,
                            inventoryDigest,
                            request.TargetOwner.OwnerId,
                            request.TargetOwner.LeaseGeneration,
                            participant.ApplicationAuthorityPayload))
                        : BuildCanonicalAuthorityPayload(
                            request,
                            envelope,
                            stored,
                            participant),
                    participant.MembershipMutation))
                .ToArray();
            var prepare = await authorityStore.PrepareAggregateAsync(
                    new ZLinkAggregatePrepareRequest(
                        request.AggregateId,
                        request.AggregateGeneration,
                        publicationParticipants,
                        inventoryDigest,
                        request.TargetDescriptor,
                        request.TargetDescriptorLifecycleGeneration,
                        request.Capacity,
                        request.TargetOwner),
                    cancellationToken)
                .ConfigureAwait(false);
            switch (prepare)
            {
                case ZLinkAggregatePrepareResult.Prepared value:
                    fence = value.Fence;
                    targetAuthorityOwnerGenerations =
                        value.TargetAuthorityOwnerGenerations;
                    break;
                case ZLinkAggregatePrepareResult.AlreadyPrepared value:
                    fence = value.Fence;
                    targetAuthorityOwnerGenerations =
                        value.TargetAuthorityOwnerGenerations;
                    break;
                case ZLinkAggregatePrepareResult.GenerationExhausted:
                    throw new ZLinkAuthorityGenerationExhaustedException(
                        "preparing an aggregate relocation");
                default:
                    throw new InvalidOperationException(
                        "The aggregate relocation prepare was rejected.");
            }

            return new ZLinkPreparedAggregateRelocation(
                fence,
                stored,
                envelope,
                request.Participants,
                inventoryDigest,
                targetAuthorityOwnerGenerations);
        }
        catch
        {
            var publication = await ProbePublicationAsync(
                    request.Participants,
                    stored,
                    envelope,
                    inventoryDigest)
                .ConfigureAwait(false);
            if (publication.State == AggregatePublicationProbe.Published)
                return new ZLinkPreparedAggregateRelocation(
                    fence,
                    stored,
                    envelope,
                    request.Participants,
                    inventoryDigest,
                    publication.TargetAuthorityOwnerGenerations);
            if (publication.State == AggregatePublicationProbe.Unknown)
                throw;

            // Prepare may fail after the provider has claimed the aggregate
            // staging row or installed participant fences. The local
            // The absence of a returned prepare result does not prove that
            // no authority state changed.
            // Delete the immutable payload only after the authority provider
            // confirms that the deterministic aggregate fence was aborted.
            var safeToDelete = false;
            try
            {
                using var abortDeadline = new CancellationTokenSource(
                    ReconciliationTimeout);
                var abort = await authorityStore.AbortAggregateAsync(
                        fence,
                        abortDeadline.Token)
                    .AsTask()
                    .WaitAsync(abortDeadline.Token)
                    .ConfigureAwait(false);
                safeToDelete = abort is ZLinkAggregateAbortResult.Aborted
                    or ZLinkAggregateAbortResult.AlreadyAborted;
            }
            catch
            {
                // An ambiguous authority state keeps the immutable root
                // available for provider reconciliation and recovery.
            }
            if (safeToDelete && ownsStoredRoot)
                await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
            throw;
        }
    }

    private static byte[] BuildCanonicalAuthorityPayload(
        ZLinkAggregateRelocationRequest request,
        ZLinkRelocationEnvelope envelope,
        ZLinkRelocationStored stored,
        ZLinkAggregateRelocationParticipant participant)
    {
        var spotParticipant = request.Participants.Single(static item =>
            item.Envelope.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        string sourceNodeRid;
        ulong sourceNodeGeneration;
        string sourceOwnerId;
        ulong sourceOwnerLease;
        if (spotParticipant.Envelope.ObjectKind
            == ZLinkPlacementObjectKind.InstanceSpot)
        {
            if (!ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    spotParticipant.ApplicationAuthorityPayload.Span,
                    out var source))
                throw new InvalidDataException(
                    "Canonical Instance SPOT source authority is invalid.");
            sourceNodeRid = source.NodeRid.ToHex();
            sourceNodeGeneration = source.NodeGeneration;
            sourceOwnerId = source.OwnerId;
            sourceOwnerLease = source.OwnerLeaseGeneration;
        }
        else
        {
            if (!ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    spotParticipant.ApplicationAuthorityPayload.Span,
                    out var source))
                throw new InvalidDataException(
                    "Canonical User SPOT source authority is invalid.");
            sourceNodeRid = source.NodeRid.ToHex();
            sourceNodeGeneration = source.NodeGeneration;
            sourceOwnerId = source.OwnerId;
            sourceOwnerLease = source.OwnerLeaseGeneration;
        }
        var basePayload = participant.ApplicationAuthorityPayload;
        if (participant.Envelope.ObjectKind == ZLinkPlacementObjectKind.Actor
            && ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                basePayload.Span,
                out var actorPhase))
            basePayload = actorPhase.ApplicationPayload;
        return ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
            basePayload.Span,
            new ZLinkCanonicalRelocationAuthorityState(
                envelope.CanonicalRelocationHigh,
                envelope.CanonicalRelocationLow,
                request.AggregateGeneration,
                sourceNodeRid,
                sourceNodeGeneration,
                sourceOwnerId,
                sourceOwnerLease,
                request.TargetDescriptor.Rid.ToHex(),
                request.TargetDescriptorLifecycleGeneration,
                request.TargetOwner.OwnerId,
                checked((ulong)request.TargetOwner.LeaseGeneration),
                request.TargetDescriptorLifecycleGeneration,
                request.TargetOwner.OwnerId,
                checked((ulong)request.TargetOwner.LeaseGeneration),
                request.TargetDescriptor.Rid.ToHex(),
                request.TargetDescriptorLifecycleGeneration,
                Phase: 4,
                stored.Reference,
                stored.ChecksumCrc32c,
                envelope.CanonicalApplicationVersion,
                SourceCleanupState: 0),
            envelope);
    }

    internal async ValueTask<ZLinkAggregateRelocationPublished> CommitAsync(
        ZLinkPreparedAggregateRelocation prepared,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(prepared);
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            await ZLinkRelocationTreeStore.RenewTreeAsync(
                    relocationStore,
                    prepared.Relocation.Reference,
                    prepared.Relocation.ChecksumCrc32c,
                    Retention,
                    cancellationToken)
                .ConfigureAwait(false);
            var commit = await authorityStore.CommitAggregateAsync(
                    prepared.Fence,
                    cancellationToken)
                .ConfigureAwait(false);
            if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "committing an aggregate relocation");
            if (commit is not (ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted))
                throw new InvalidOperationException(
                    $"The aggregate relocation commit failed with '{commit}'.");
        }
        catch
        {
            if ((await ProbePublicationAsync(
                    prepared.Participants,
                    prepared.Relocation,
                    prepared.Envelope,
                    prepared.InventoryDigest)
                    .ConfigureAwait(false)).State
                != AggregatePublicationProbe.Published)
                throw;
        }

        return new ZLinkAggregateRelocationPublished(
            prepared.Fence,
            prepared.Relocation,
            prepared.Envelope);
    }

    internal ValueTask<ZLinkRelocationEnvelope> AdvanceCanonicalReplayAsync(
        ZLinkRelocationEnvelope identity,
        ulong participantId,
        ulong acceptedSequence,
        ZLinkCanonicalTerminalCompletion? completion,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        return UpdateCanonicalRootAsync(
            identity,
            current =>
            {
                var participant = current.Participants.Single(candidate =>
                    candidate.CanonicalParticipantId == participantId);
                if (participant.ReplayCursor >= acceptedSequence)
                    return current;
                var successor = current;
                if (completion is not null)
                {
                    var exists = current.Participants
                        .SelectMany(static item => item.TerminalCompletions)
                        .Any(item => SameCompletion(item, completion));
                    if (!exists)
                        successor = ZLinkRelocationEnvelopeCodec
                            .AppendCanonicalTerminalCompletion(
                                successor,
                                completion);
                }
                return ZLinkRelocationEnvelopeCodec.AdvanceCanonicalReplayCursor(
                    successor,
                    participantId,
                    acceptedSequence);
            },
            targetDescriptor,
            targetDescriptorLifecycleGeneration,
            targetOwner,
            cancellationToken);
    }

    internal ValueTask<ZLinkRelocationEnvelope> AcknowledgeCanonicalReplyAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkCanonicalTerminalCompletion completion,
        byte deliveryState,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(completion);
        return UpdateCanonicalRootAsync(
            identity,
            current => ZLinkRelocationEnvelopeCodec
                .CompleteCanonicalTerminalDelivery(
                    current,
                    completion.OperationHigh,
                    completion.OperationLow,
                    completion.SourceOwnerId,
                    completion.SourceOwnerLeaseGeneration,
                    completion.SourceNodeRid,
                    completion.SourceNodeGeneration,
                    deliveryState),
            targetDescriptor,
            targetDescriptorLifecycleGeneration,
            targetOwner,
            cancellationToken);
    }

    internal ValueTask<ZLinkRelocationEnvelope>
        ExpireCanonicalReplySourceLeaseAsync(
            ZLinkRelocationEnvelope identity,
            ZLinkCanonicalTerminalCompletion completion,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            ulong targetDescriptorLifecycleGeneration,
            ZLinkLocationOwnerToken targetOwner,
            CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(completion);
        return UpdateCanonicalRootAsync(
            identity,
            current => ZLinkRelocationEnvelopeCodec
                .ExpireCanonicalTerminalSourceLease(
                    current,
                    completion.OperationHigh,
                    completion.OperationLow,
                    completion.SourceOwnerId,
                    completion.SourceOwnerLeaseGeneration,
                    completion.SourceNodeRid,
                    completion.SourceNodeGeneration),
            targetDescriptor,
            targetDescriptorLifecycleGeneration,
            targetOwner,
            cancellationToken);
    }

    internal async ValueTask<ZLinkRelocationEnvelope> ReadCanonicalRootAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        var current = await ReadCanonicalProgressAsync(
                identity,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        return current.Root;
    }

    private async ValueTask<ZLinkRelocationEnvelope> UpdateCanonicalRootAsync(
        ZLinkRelocationEnvelope identity,
        Func<ZLinkRelocationEnvelope, ZLinkRelocationEnvelope> update,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(identity);
        ArgumentNullException.ThrowIfNull(update);
        if (identity.CanonicalLogicalStream.IsEmpty)
            throw new ArgumentException(
                "Replay progress requires a canonical relocation root.",
                nameof(identity));

        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var current = await ReadCanonicalProgressAsync(
                    identity,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            var successor = update(current.Root);
            if (successor.CanonicalLogicalStream.Span.SequenceEqual(
                    current.Root.CanonicalLogicalStream.Span))
                return current.Root;

            var storedTree = await ZLinkRelocationTreeStore.PutAsync(
                    relocationStore,
                    successor,
                    Retention,
                    cancellationToken)
                .ConfigureAwait(false);
            var stored = storedTree.Root;
            var mutations = current.Authorities.Select(entry =>
                    new ZLinkAggregateParticipant(
                        entry.Participant.AuthorityKey,
                        entry.Snapshot.StoreVersion,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        ZLinkCanonicalRelocationAuthorityStateCodec
                            .ReplaceRelocationState(
                                entry.Snapshot.Payload.Span,
                                entry.Projection.State with
                                {
                                    RelocationReference = stored.Reference,
                                    RelocationChecksumCrc32c =
                                        stored.ChecksumCrc32c
                                },
                                successor),
                        ReadOnlyMemory<byte>.Empty))
                .ToArray();
            var generation = CanonicalMutationGeneration(
                successor.CanonicalLogicalStream.Span,
                current.Projection.SourceCleanupState);
            var prepare = await authorityStore.PrepareAggregateAsync(
                    new ZLinkAggregatePrepareRequest(
                        identity.AggregateId,
                        generation,
                        mutations,
                        identity.InventoryDigest,
                        targetDescriptor,
                        targetDescriptorLifecycleGeneration,
                        new ZLinkCapacityVector(0, 0, null),
                        targetOwner),
                    cancellationToken)
                .ConfigureAwait(false);
            if (prepare is ZLinkAggregatePrepareResult.GenerationExhausted)
            {
                await ZLinkRelocationTreeStore.DeleteTreeAsync(
                        relocationStore,
                        stored.Reference,
                        CancellationToken.None)
                    .ConfigureAwait(false);
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "preparing canonical relocation progress");
            }
            if (prepare is ZLinkAggregatePrepareResult.Conflict
                or ZLinkAggregatePrepareResult.Stale)
            {
                await ZLinkRelocationTreeStore.DeleteTreeAsync(
                        relocationStore,
                        stored.Reference,
                        CancellationToken.None)
                    .ConfigureAwait(false);
                continue;
            }
            if (prepare is not (
                    ZLinkAggregatePrepareResult.Prepared
                    or ZLinkAggregatePrepareResult.AlreadyPrepared))
                throw new InvalidOperationException(
                    "The authority store returned an invalid canonical relocation prepare result.");
            var fence = prepare switch
            {
                ZLinkAggregatePrepareResult.Prepared value => value.Fence,
                ZLinkAggregatePrepareResult.AlreadyPrepared value => value.Fence,
                _ => throw new InvalidOperationException()
            };
            var commit = await authorityStore.CommitAggregateAsync(
                    fence,
                    cancellationToken)
                .ConfigureAwait(false);
            if (commit is ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted)
            {
                CacheCanonicalRoot(stored, successor);
                return successor;
            }
            if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "committing canonical relocation progress");
            if (commit != ZLinkAggregateCommitResult.Stale)
                throw new InvalidOperationException(
                    "The authority store returned an invalid canonical relocation commit result.");
        }
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "Canonical relocation progress conflicted after the bounded retry limit.",
            ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private async ValueTask<CanonicalProgress> ReadCanonicalProgressAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var authorities = new List<CanonicalProgressAuthority>(
                identity.Participants.Count);
            ZLinkCanonicalRelocationAuthorityProjection? shared = null;
            ZLinkAuthorityKey? sharedKey = null;
            (ZLinkAuthorityKey Key,
                ZLinkCanonicalRelocationAuthorityProjection Projection)?
                mismatch = null;
            var orderedParticipants = identity.Participants.OrderBy(
                    static participant =>
                        participant.CanonicalParticipantId)
                .ToArray();
            var participantReads = await Task.WhenAll(
                    orderedParticipants.Select(async participant => (
                        Participant: participant,
                        Read: await authorityStore.ReadAuthorityAsync(
                                participant.AuthorityKey,
                                cancellationToken)
                            .ConfigureAwait(false))))
                .ConfigureAwait(false);
            foreach (var (participant, read) in participantReads)
            {
                if (read is not ZLinkAuthorityReadResult.Found found
                    || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        found.Snapshot.Payload.Span,
                        out var projection)
                    || projection.RelocationHigh
                    != identity.CanonicalRelocationHigh
                    || projection.RelocationLow
                    != identity.CanonicalRelocationLow
                    || projection.TargetOwnerId != targetOwner.OwnerId
                    || projection.TargetOwnerLeaseGeneration
                    != checked((ulong)targetOwner.LeaseGeneration))
                    throw new ZLinkRelocationDataLostException(
                        $"Canonical relocation authority '{participant.AuthorityKey.Value}' changed during replay.");
                if (shared is not null
                    && !SameCanonicalProgress(shared, projection))
                    mismatch ??= (participant.AuthorityKey, projection);
                shared ??= projection;
                sharedKey ??= participant.AuthorityKey;
                authorities.Add(new CanonicalProgressAuthority(
                    participant,
                    found.Snapshot,
                    projection));
            }

            var currentProjection = shared
                                    ?? throw new ZLinkRelocationDataLostException(
                                        "Canonical relocation has no authority participants.");
            var anchorRead = await authorityStore.ReadAuthorityAsync(
                    sharedKey!.Value,
                    cancellationToken)
                .ConfigureAwait(false);
            if (anchorRead is not ZLinkAuthorityReadResult.Found anchorFound
                || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    anchorFound.Snapshot.Payload.Span,
                    out var anchorProjection)
                || anchorProjection.RelocationHigh
                != identity.CanonicalRelocationHigh
                || anchorProjection.RelocationLow
                != identity.CanonicalRelocationLow
                || anchorProjection.TargetOwnerId != targetOwner.OwnerId
                || anchorProjection.TargetOwnerLeaseGeneration
                != checked((ulong)targetOwner.LeaseGeneration))
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authority '{sharedKey.Value.Value}' changed during replay.");

            // Participant reads are separate store operations. A concurrent
            // aggregate publication can therefore put the scan across two
            // valid immutable roots. Re-reading the first participant tells
            // that transient view apart from a stable split publication.
            if (!SameCanonicalProgress(currentProjection, anchorProjection))
            {
                await Task.Yield();
                continue;
            }
            if (mismatch is { } different)
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authorities '{sharedKey.Value.Value}' and '{different.Key.Value}' expose different replay progress "
                    + $"(reference {currentProjection.RelocationReference}/{different.Projection.RelocationReference}, "
                    + $"completion {currentProjection.TerminalCompletionCount}/{different.Projection.TerminalCompletionCount}, "
                    + $"pending {currentProjection.PendingRelayCount}/{different.Projection.PendingRelayCount}, "
                    + $"cleanup {currentProjection.SourceCleanupState}/{different.Projection.SourceCleanupState}).");

            var root = TryGetCachedCanonicalRoot(
                currentProjection.RelocationReference,
                currentProjection.RelocationChecksumCrc32c);
            root ??= await ZLinkRelocationTreeStore.GetAsync(
                        relocationStore,
                        currentProjection.RelocationReference,
                        currentProjection.RelocationChecksumCrc32c,
                        cancellationToken)
                    .ConfigureAwait(false);
            if (root.CanonicalRelocationHigh
                    != identity.CanonicalRelocationHigh
                || root.CanonicalRelocationLow
                    != identity.CanonicalRelocationLow
                || root.Participants.Count != identity.Participants.Count)
                throw new ZLinkRelocationDataLostException(
                    "Canonical replay root does not match its authority inventory.");
            var identities = identity.Participants.ToDictionary(
                static participant => participant.CanonicalParticipantId);
            root = root with
            {
                Participants = root.Participants.Select(state =>
                {
                    if (!identities.TryGetValue(
                            state.CanonicalParticipantId,
                            out var participant))
                        throw new ZLinkRelocationDataLostException(
                            "Canonical replay root contains an unknown participant.");
                    return state with
                    {
                        AuthorityKey = participant.AuthorityKey,
                        ObjectKind = participant.ObjectKind,
                        ObjectGeneration = participant.ObjectGeneration,
                        AuthorityOwnerGeneration =
                            participant.AuthorityOwnerGeneration
                    };
                }).ToArray()
            };
            return new CanonicalProgress(
                root,
                currentProjection,
                authorities);
        }

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "Canonical replay progress changed throughout the bounded read window.",
            ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private static bool SameCanonicalProgress(
        ZLinkCanonicalRelocationAuthorityProjection left,
        ZLinkCanonicalRelocationAuthorityProjection right) =>
        left.RelocationReference == right.RelocationReference
        && left.RelocationChecksumCrc32c == right.RelocationChecksumCrc32c
        && left.TerminalCompletionCount == right.TerminalCompletionCount
        && left.PendingRelayCount == right.PendingRelayCount
        && left.SourceCleanupState == right.SourceCleanupState;

    private ZLinkRelocationEnvelope? TryGetCachedCanonicalRoot(
        string reference,
        uint checksum)
    {
        lock (canonicalRootCacheGate)
            return canonicalRootCacheReference == reference
                   && canonicalRootCacheChecksum == checksum
                ? canonicalRootCache
                : null;
    }

    private void CacheCanonicalRoot(
        ZLinkRelocationStored stored,
        ZLinkRelocationEnvelope root)
    {
        lock (canonicalRootCacheGate)
        {
            canonicalRootCacheReference = stored.Reference;
            canonicalRootCacheChecksum = stored.ChecksumCrc32c;
            canonicalRootCache = root;
        }
    }

    private static ulong CanonicalMutationGeneration(
        ReadOnlySpan<byte> logicalRoot,
        byte sourceCleanupState)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData(logicalRoot);
        hash.AppendData([sourceCleanupState]);
        var value = BinaryPrimitives.ReadUInt64BigEndian(hash.GetHashAndReset())
                    & long.MaxValue;
        return value == 0 ? 1 : value;
    }

    private static bool SameCompletion(
        ZLinkCanonicalTerminalCompletion left,
        ZLinkCanonicalTerminalCompletion right) =>
        left.OperationHigh == right.OperationHigh
        && left.OperationLow == right.OperationLow
        && left.SourceOwnerId == right.SourceOwnerId
        && left.SourceOwnerLeaseGeneration == right.SourceOwnerLeaseGeneration
        && left.SourceNodeRid == right.SourceNodeRid
        && left.SourceNodeGeneration == right.SourceNodeGeneration;

    private sealed record CanonicalProgress(
        ZLinkRelocationEnvelope Root,
        ZLinkCanonicalRelocationAuthorityProjection Projection,
        IReadOnlyList<CanonicalProgressAuthority> Authorities);

    private sealed record CanonicalProgressAuthority(
        ZLinkRelocationParticipantEnvelope Participant,
        ZLinkAuthoritySnapshot Snapshot,
        ZLinkCanonicalRelocationAuthorityProjection Projection);

    internal async ValueTask<bool> TryCompleteSourceCleanupAsync(
        ZLinkAggregateRelocationPublished published,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(published);
        var spot = published.Envelope.Participants.Single(
            static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot);
        if (!published.Envelope.CanonicalLogicalStream.IsEmpty)
        {
            return await TryCompleteCanonicalSourceCleanupAsync(
                    published,
                    targetDescriptor,
                    targetDescriptorLifecycleGeneration,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (!ZLinkSpotRetireCompletionMarker.IsPending(
                spot.CompletionPayload.Span))
            throw new InvalidOperationException(
                "The initial relocation root is not source-cleanup pending.");

        var completedEnvelope = published.Envelope with
        {
            AggregateGeneration = checked(
                published.Envelope.AggregateGeneration + 1),
            Participants = published.Envelope.Participants.Select(
                    participant => participant.AuthorityKey
                                   == spot.AuthorityKey
                        ? participant with
                        {
                            CompletionPayload =
                                ZLinkSpotRetireCompletionMarker
                                    .CreateCompleted()
                        }
                        : participant)
                .ToArray()
        };
        var completedTree = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                completedEnvelope,
                Retention,
                cancellationToken)
            .ConfigureAwait(false);
        var completed = completedTree.Root;
        var mutations = new List<ZLinkAggregateParticipant>(
            completedEnvelope.Participants.Count);
        var alreadyCompleted = true;
        foreach (var participant in completedEnvelope.Participants)
        {
            var read = await authorityStore.ReadAuthorityAsync(
                    participant.AuthorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw new ZLinkRelocationDataLostException(
                    $"Relocation authority '{participant.AuthorityKey.Value}' disappeared before source cleanup completion.");
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    found.Snapshot.Payload.Span,
                    out var current))
            {
                // A retry after target ACK can observe the steady payload.
                continue;
            }
            if (current.AggregateId != completedEnvelope.AggregateId
                || current.TargetOwnerId != targetOwner.OwnerId
                || current.TargetOwnerLeaseGeneration
                   != targetOwner.LeaseGeneration)
                throw new ZLinkRelocationDataLostException(
                    $"Relocation authority '{participant.AuthorityKey.Value}' has a different completion fence.");
            if (current.AggregateGeneration
                    == completedEnvelope.AggregateGeneration
                && current.Reference == completed.Reference
                && current.ChecksumCrc32c == completed.ChecksumCrc32c)
                continue;
            alreadyCompleted = false;
            if (current.AggregateGeneration
                    != published.Envelope.AggregateGeneration
                || current.Reference != published.Relocation.Reference
                || current.ChecksumCrc32c
                   != published.Relocation.ChecksumCrc32c)
                throw new ZLinkRelocationDataLostException(
                    $"Relocation authority '{participant.AuthorityKey.Value}' is not source-cleanup pending.");
            mutations.Add(new ZLinkAggregateParticipant(
                participant.AuthorityKey,
                found.Snapshot.StoreVersion,
                ZLinkAuthorityGenerationTransition.Preserve,
                ZLinkRelocationAuthorityPayloadCodec.Encode(
                    current with
                    {
                        Reference = completed.Reference,
                        ChecksumCrc32c = completed.ChecksumCrc32c,
                        AggregateGeneration =
                            completedEnvelope.AggregateGeneration
                    }),
                ReadOnlyMemory<byte>.Empty));
        }
        if (alreadyCompleted || mutations.Count == 0)
            return true;
        if (mutations.Count != completedEnvelope.Participants.Count)
            throw new ZLinkRelocationDataLostException(
                "Relocation source-cleanup completion is partially visible.");

        await ZLinkRelocationTreeStore.RenewTreeAsync(
                relocationStore,
                completed.Reference,
                completed.ChecksumCrc32c,
                Retention,
                cancellationToken)
            .ConfigureAwait(false);
        var prepare = await authorityStore.PrepareAggregateAsync(
                new ZLinkAggregatePrepareRequest(
                    completedEnvelope.AggregateId,
                    completedEnvelope.AggregateGeneration,
                    mutations,
                    completedEnvelope.InventoryDigest,
                    targetDescriptor,
                    targetDescriptorLifecycleGeneration,
                    new ZLinkCapacityVector(0, 0, null),
                    targetOwner),
                cancellationToken)
            .ConfigureAwait(false);
        var fence = prepare switch
        {
            ZLinkAggregatePrepareResult.Prepared value => value.Fence,
            ZLinkAggregatePrepareResult.AlreadyPrepared value => value.Fence,
            ZLinkAggregatePrepareResult.GenerationExhausted =>
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "preparing source-cleanup completion"),
            _ => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Source-cleanup completion authority prepare conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff)
        };
        var commit = await authorityStore.CommitAggregateAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
        if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "committing source-cleanup completion");
        if (commit is not (
                ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Source-cleanup completion authority commit failed.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        await DeleteOrphanAsync(published.Relocation.Reference)
            .ConfigureAwait(false);
        return true;
    }

    private async ValueTask<bool> TryCompleteCanonicalSourceCleanupAsync(
        ZLinkAggregateRelocationPublished published,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        var expectedRequests = checked((uint)published.Envelope.Participants
            .SelectMany(static participant => participant.AcceptedJobs)
            .Count(static job => job.CanonicalRequest?.ReplyRouteId > 0));
        var currentAuthorities = new List<CanonicalProgressAuthority>(
            published.Envelope.Participants.Count);
        ZLinkCanonicalRelocationAuthorityProjection? shared = null;
        var replyDeliveryPending = false;
        var steadyAuthorities = 0;
        foreach (var participant in published.Envelope.Participants)
        {
            var read = await authorityStore.ReadAuthorityAsync(
                    participant.AuthorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authority '{participant.AuthorityKey.Value}' disappeared during source cleanup.");
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var current))
            {
                if (!IsExactNormalizedTargetAuthority(
                        found.Snapshot,
                        participant,
                        published.Envelope.AggregateId,
                        targetDescriptor,
                        targetDescriptorLifecycleGeneration,
                        targetOwner))
                    throw new ZLinkRelocationDataLostException(
                        $"Canonical relocation authority '{participant.AuthorityKey.Value}' has a different source-cleanup fence.");
                steadyAuthorities++;
                continue;
            }
            if (steadyAuthorities != 0)
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation source-cleanup completion is partially normalized.");
            if (current.RelocationHigh
                   != published.Envelope.CanonicalRelocationHigh
                || current.RelocationLow
                   != published.Envelope.CanonicalRelocationLow
                || current.TargetAttemptGeneration
                   != published.Envelope.AggregateGeneration
                || current.TargetOwnerId != targetOwner.OwnerId
                || current.TargetOwnerLeaseGeneration
                   != checked((ulong)targetOwner.LeaseGeneration))
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authority '{participant.AuthorityKey.Value}' has a different source-cleanup fence.");
            if (shared is not null
                && (shared.RelocationReference != current.RelocationReference
                    || shared.RelocationChecksumCrc32c
                       != current.RelocationChecksumCrc32c
                    || shared.TerminalCompletionCount
                       != current.TerminalCompletionCount
                    || shared.PendingRelayCount != current.PendingRelayCount
                    || shared.SourceCleanupState
                       != current.SourceCleanupState))
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation authorities expose different source-cleanup progress.");
            shared ??= current;
            if (current.PendingRelayCount != 0
                || current.TerminalCompletionCount != expectedRequests)
            {
                if (current.SourceCleanupState != 0
                    || current.Phase is < 4 or > 7)
                    throw new ZLinkRelocationDataLostException(
                        $"Canonical relocation authority '{participant.AuthorityKey.Value}' has invalid pending reply progress.");
                replyDeliveryPending = true;
                continue;
            }
            if (current.SourceCleanupState == 1 && current.Phase == 8)
                continue;
            if (current.SourceCleanupState != 0
                || current.Phase is < 4 or > 7)
                throw new ZLinkRelocationDataLostException(
                    $"Canonical relocation authority '{participant.AuthorityKey.Value}' has an invalid source-cleanup phase.");
            currentAuthorities.Add(new CanonicalProgressAuthority(
                participant,
                found.Snapshot,
                current));
        }
        if (steadyAuthorities != 0)
        {
            if (steadyAuthorities != published.Envelope.Participants.Count)
                throw new ZLinkRelocationDataLostException(
                    "Canonical relocation source-cleanup completion is partially normalized.");
            return true;
        }
        if (replyDeliveryPending)
            return false;
        if (currentAuthorities.Count == 0)
            return true;
        if (currentAuthorities.Count != published.Envelope.Participants.Count)
            throw new ZLinkRelocationDataLostException(
                "Canonical relocation source-cleanup completion is partially visible.");

        var currentProjection = shared
                                ?? throw new ZLinkRelocationDataLostException(
                                    "Canonical relocation source-cleanup authority is unavailable.");
        var currentRoot = await ZLinkRelocationTreeStore.GetAsync(
                relocationStore,
                currentProjection.RelocationReference,
                currentProjection.RelocationChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        var mutations = currentAuthorities.Select(entry =>
            new ZLinkAggregateParticipant(
                entry.Participant.AuthorityKey,
                entry.Snapshot.StoreVersion,
                ZLinkAuthorityGenerationTransition.Preserve,
                ZLinkCanonicalRelocationAuthorityStateCodec
                    .ReplaceRelocationState(
                        entry.Snapshot.Payload.Span,
                        entry.Projection.State with
                        {
                            Phase = 8,
                            SourceCleanupState = 1
                        },
                        currentRoot),
                ReadOnlyMemory<byte>.Empty)).ToArray();

        await ZLinkRelocationTreeStore.RenewTreeAsync(
                relocationStore,
                currentProjection.RelocationReference,
                currentProjection.RelocationChecksumCrc32c,
                Retention,
                cancellationToken)
            .ConfigureAwait(false);
        var completionGeneration = CanonicalMutationGeneration(
            currentRoot.CanonicalLogicalStream.Span,
            sourceCleanupState: 1);
        var prepare = await authorityStore.PrepareAggregateAsync(
                new ZLinkAggregatePrepareRequest(
                    published.Envelope.AggregateId,
                    completionGeneration,
                    mutations,
                    published.Envelope.InventoryDigest,
                    targetDescriptor,
                    targetDescriptorLifecycleGeneration,
                    new ZLinkCapacityVector(0, 0, null),
                    targetOwner),
                cancellationToken)
            .ConfigureAwait(false);
        var fence = prepare switch
        {
            ZLinkAggregatePrepareResult.Prepared value => value.Fence,
            ZLinkAggregatePrepareResult.AlreadyPrepared value => value.Fence,
            ZLinkAggregatePrepareResult.GenerationExhausted =>
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "preparing canonical source-cleanup completion"),
            _ => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical source-cleanup completion authority prepare conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff)
        };
        var commit = await authorityStore.CommitAggregateAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
        if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "committing canonical source-cleanup completion");
        if (commit is not (
                ZLinkAggregateCommitResult.Committed
                or ZLinkAggregateCommitResult.AlreadyCommitted))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Canonical source-cleanup completion authority commit failed.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        return true;
    }

    internal static bool IsExactNormalizedTargetAuthority(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkRelocationParticipantEnvelope participant,
        Guid aggregateId,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        bool allowCurrentAuthorityGeneration = false)
    {
        if (ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out _)
            || snapshot.Allocation.ObjectKind != participant.ObjectKind
            || snapshot.Allocation.Descriptor != targetDescriptor
            || snapshot.Allocation.DescriptorLifecycleGeneration
               != targetDescriptorLifecycleGeneration
            || snapshot.OwnerId != targetOwner.OwnerId
            || snapshot.OwnerLeaseGeneration != targetOwner.LeaseGeneration
            || snapshot.ObjectGeneration != participant.ObjectGeneration
            || participant.AuthorityOwnerGeneration is 0 or > long.MaxValue
            || (allowCurrentAuthorityGeneration
                ? snapshot.AuthorityOwnerGeneration
                  < participant.AuthorityOwnerGeneration
                : snapshot.AuthorityOwnerGeneration
                  <= participant.AuthorityOwnerGeneration))
            return false;

        ZLinkCanonicalParticipantRecovery recovery;
        try
        {
            recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                participant.RecoveryPayload.Span);
        }
        catch (Exception error) when (error is InvalidDataException
                                      or ArgumentException
                                      or OverflowException)
        {
            return false;
        }
        if (recovery.AuthorityKey != participant.AuthorityKey
            || recovery.ObjectKind != participant.ObjectKind
            || recovery.ObjectGeneration != participant.ObjectGeneration
            || recovery.AuthorityOwnerGeneration
               != participant.AuthorityOwnerGeneration
            || snapshot.Allocation.StableType != recovery.StableType)
            return false;

        var expected = ZLinkSpotRetireTargetRuntime.BuildTargetReadyPayload(
            participant.ObjectKind,
            recovery.AuthorityPayload,
            targetDescriptor.Rid,
            targetDescriptorLifecycleGeneration,
            targetOwner,
            aggregateId);
        return snapshot.Payload.Span.SequenceEqual(expected.Span);
    }

    internal async ValueTask AbortAsync(
        ZLinkPreparedAggregateRelocation prepared)
    {
        ArgumentNullException.ThrowIfNull(prepared);
        var abort = await authorityStore.AbortAggregateAsync(
                prepared.Fence,
                CancellationToken.None)
            .ConfigureAwait(false);
        if (abort is not (ZLinkAggregateAbortResult.Aborted
            or ZLinkAggregateAbortResult.AlreadyAborted))
            throw new InvalidOperationException(
                $"The aggregate relocation abort failed with '{abort}'.");
        await DeleteOrphanAsync(prepared.Relocation.Reference)
            .ConfigureAwait(false);
    }

    private async ValueTask<AggregatePublicationProbeResult>
        ProbePublicationAsync(
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants,
        ZLinkRelocationStored stored,
        ZLinkRelocationEnvelope envelope,
        ReadOnlyMemory<byte> inventoryDigest)
    {
        var results = new AggregatePublicationProbe[participants.Count];
        var targetAuthorityOwnerGenerations =
            new ulong[participants.Count];
        using var deadline = new CancellationTokenSource(
            ReconciliationTimeout);
        try
        {
            await Parallel.ForEachAsync(
                Enumerable.Range(0, participants.Count),
                new ParallelOptions
                {
                    MaxDegreeOfParallelism =
                        MaxPublicationProbeConcurrency,
                    CancellationToken = deadline.Token
                },
                async (index, token) =>
                {
                    var participant = participants[index];
                    var read = await authorityStore.ReadAuthorityAsync(
                            participant.Envelope.AuthorityKey,
                            token)
                        .ConfigureAwait(false);
                    if (read is ZLinkAuthorityReadResult.Found found
                        && MatchesPublication(
                            found,
                            stored,
                            envelope,
                            inventoryDigest)
                        && found.Snapshot.AuthorityOwnerGeneration
                           is > 0 and <= long.MaxValue)
                    {
                        results[index] = AggregatePublicationProbe.Published;
                        targetAuthorityOwnerGenerations[index] =
                            found.Snapshot.AuthorityOwnerGeneration;
                    }
                    else
                    {
                        results[index] =
                            AggregatePublicationProbe.NotPublished;
                    }
                }).ConfigureAwait(false);
        }
        catch
        {
            return AggregatePublicationProbeResult.Unknown;
        }

        if (results.All(static result =>
                result == AggregatePublicationProbe.Published))
        {
            var generations = new Dictionary<ZLinkAuthorityKey, ulong>(
                participants.Count);
            for (var index = 0; index < participants.Count; index++)
                generations.Add(
                    participants[index].Envelope.AuthorityKey,
                    targetAuthorityOwnerGenerations[index]);
            return new AggregatePublicationProbeResult(
                AggregatePublicationProbe.Published,
                generations);
        }
        if (results.All(static result =>
                result == AggregatePublicationProbe.NotPublished))
            return AggregatePublicationProbeResult.NotPublished;
        return AggregatePublicationProbeResult.Unknown;

        static bool MatchesPublication(
            ZLinkAuthorityReadResult.Found found,
            ZLinkRelocationStored stored,
            ZLinkRelocationEnvelope envelope,
            ReadOnlyMemory<byte> inventoryDigest)
        {
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    found.Snapshot.Payload.Span,
                    out var publication)
                || !string.Equals(
                    publication.Reference,
                    stored.Reference,
                    StringComparison.Ordinal)
                || publication.ChecksumCrc32c != stored.ChecksumCrc32c
                || publication.AggregateId != envelope.AggregateId
                || publication.AggregateGeneration
                   != envelope.AggregateGeneration)
                return false;
            if (envelope.CanonicalLogicalStream.IsEmpty)
            {
                return !publication.IsCanonical
                       && publication.InventoryDigest.Span.SequenceEqual(
                           inventoryDigest.Span);
            }
            return publication.IsCanonical;
        }
    }

    private enum AggregatePublicationProbe
    {
        Unknown = 0,
        Published = 1,
        NotPublished = 2
    }

    private sealed record AggregatePublicationProbeResult(
        AggregatePublicationProbe State,
        IReadOnlyDictionary<ZLinkAuthorityKey, ulong>
            TargetAuthorityOwnerGenerations)
    {
        internal static AggregatePublicationProbeResult Unknown { get; } =
            new(
                AggregatePublicationProbe.Unknown,
                new Dictionary<ZLinkAuthorityKey, ulong>());

        internal static AggregatePublicationProbeResult NotPublished { get; } =
            new(
                AggregatePublicationProbe.NotPublished,
                new Dictionary<ZLinkAuthorityKey, ulong>());
    }

    private async ValueTask DeleteOrphanAsync(string reference)
    {
        try
        {
            await ZLinkRelocationTreeStore.DeleteTreeAsync(
                    relocationStore,
                    reference,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
        }
    }

    private static void Validate(ZLinkAggregateRelocationRequest request)
    {
        if (request.AggregateId == Guid.Empty)
            throw new ArgumentException(
                "The aggregate id must not be empty.",
                nameof(request));
        if (request.AggregateGeneration is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(request));
        if (request.TargetOwner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(request));
        if (request.Participants.Count < 1)
            throw new ArgumentOutOfRangeException(nameof(request));
        var keys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var participant in request.Participants)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(
                participant.ExpectedStoreVersion);
            if (!keys.Add(participant.Envelope.AuthorityKey.Value))
                throw new ArgumentException(
                    "Aggregate participant keys must be unique.",
                    nameof(request));
        }
    }

}

internal static class ZLinkAggregateInventoryDigest
{
    internal static byte[] Compute(
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        foreach (var participant in participants.OrderBy(
                     static value => value.Envelope.AuthorityKey.Value,
                     StringComparer.Ordinal))
        {
            WriteString(writer, participant.Envelope.AuthorityKey.Value);
            writer.Write((byte)participant.Envelope.ObjectKind);
            writer.Write(participant.Envelope.ObjectGeneration);
            writer.Write(participant.Envelope.AuthorityOwnerGeneration);
            WriteString(writer, participant.ExpectedStoreVersion);
            writer.Write((byte)participant.OwnerTransition);
            WriteBytes(writer, participant.ApplicationAuthorityPayload.Span);
            WriteBytes(writer, participant.MembershipMutation.Span);
        }
        writer.Flush();
        return SHA256.HashData(stream.GetBuffer().AsSpan(0, checked((int)stream.Length)));
    }

    private static void WriteString(BinaryWriter writer, string value)
    {
        var encoded = Encoding.UTF8.GetBytes(value);
        writer.Write(encoded.Length);
        writer.Write(encoded);
    }

    private static void WriteBytes(BinaryWriter writer, ReadOnlySpan<byte> value)
    {
        writer.Write(value.Length);
        writer.Write(value);
    }
}
