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

internal sealed class ZLinkAuthorityGenerationExhaustedException(string operation)
    : InvalidOperationException(
        $"Authority generation was exhausted while {operation}.");

internal sealed class ZLinkAggregateRelocationCoordinator(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore)
{
    private sealed record PublicationState(
        ZLinkAggregateFence Fence,
        ZLinkRelocationStored Relocation,
        ZLinkRelocationEnvelope Envelope,
        IReadOnlyList<ZLinkAggregateRelocationParticipant> Participants,
        ReadOnlyMemory<byte> InventoryDigest);

    private const int MaxConflictRetries = 8;
    private const int MaxPublicationProbeConcurrency = 64;
    private static readonly TimeSpan Retention = TimeSpan.FromHours(24);
    private static readonly TimeSpan ReconciliationTimeout =
        TimeSpan.FromSeconds(5);
    internal async ValueTask<ZLinkAggregateRelocationPublished> PublishAsync(
        ZLinkAggregateRelocationRequest request,
        CancellationToken cancellationToken = default,
        ZLinkRelocationStored? acceptedRelocation = null)
    {
        var publication = await PreparePublicationAsync(
                request,
                acceptedRelocation,
                cancellationToken)
            .ConfigureAwait(false);
        return await CommitPublicationAsync(publication, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<PublicationState> PreparePublicationAsync(
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
                    break;
                case ZLinkAggregatePrepareResult.AlreadyPrepared value:
                    fence = value.Fence;
                    break;
                case ZLinkAggregatePrepareResult.GenerationExhausted:
                    throw new ZLinkAuthorityGenerationExhaustedException(
                        "preparing an aggregate relocation");
                default:
                    throw new InvalidOperationException(
                        "The aggregate relocation prepare was rejected.");
            }

            return new PublicationState(
                fence,
                stored,
                envelope,
                request.Participants,
                inventoryDigest);
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
                return new PublicationState(
                    fence,
                    stored,
                    envelope,
                    request.Participants,
                    inventoryDigest);
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
                request.TargetOwner.OwnerId,
                checked((ulong)request.TargetOwner.LeaseGeneration),
                request.TargetDescriptor.Rid.ToHex(),
                request.TargetDescriptorLifecycleGeneration,
                Phase: 4,
                envelope.CanonicalApplicationVersion)
            {
                CoordinatorExpectedAuthorityStoreVersion =
                    spotParticipant.ExpectedStoreVersion,
                RelocationReference = stored.Reference,
                RelocationChecksumCrc32c = stored.ChecksumCrc32c
            },
            envelope);
    }

    private async ValueTask<ZLinkAggregateRelocationPublished>
        CommitPublicationAsync(
        PublicationState publication,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            await ZLinkRelocationTreeStore.RenewTreeAsync(
                    relocationStore,
                    publication.Relocation.Reference,
                    publication.Relocation.ChecksumCrc32c,
                    Retention,
                    cancellationToken)
                .ConfigureAwait(false);
            var commit = await authorityStore.CommitAggregateAsync(
                    publication.Fence,
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
                    publication.Participants,
                    publication.Relocation,
                    publication.Envelope,
                    publication.InventoryDigest)
                    .ConfigureAwait(false)).State
                != AggregatePublicationProbe.Published)
                throw;
        }

        return new ZLinkAggregateRelocationPublished(
            publication.Fence,
            publication.Relocation,
            publication.Envelope);
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

    private async ValueTask<CanonicalProgress> ReadCanonicalProgressAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
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
                    $"Canonical relocation authorities '{sharedKey.Value.Value}' and '{different.Key.Value}' expose different relocation state "
                    + $"(reference {currentProjection.RelocationReference}/{different.Projection.RelocationReference}).");

            var root = await ZLinkRelocationTreeStore.GetAsync(
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
                currentProjection);
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
        && left.RelocationChecksumCrc32c == right.RelocationChecksumCrc32c;

    private sealed record CanonicalProgress(
        ZLinkRelocationEnvelope Root,
        ZLinkCanonicalRelocationAuthorityProjection Projection);

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

    private async ValueTask<AggregatePublicationProbeResult>
        ProbePublicationAsync(
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants,
        ZLinkRelocationStored stored,
        ZLinkRelocationEnvelope envelope,
        ReadOnlyMemory<byte> inventoryDigest)
    {
        var results = new AggregatePublicationProbe[participants.Count];
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
                            inventoryDigest))
                    {
                        results[index] = AggregatePublicationProbe.Published;
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
            return AggregatePublicationProbeResult.Published;
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
        AggregatePublicationProbe State)
    {
        internal static AggregatePublicationProbeResult Unknown { get; } =
            new(AggregatePublicationProbe.Unknown);

        internal static AggregatePublicationProbeResult Published { get; } =
            new(AggregatePublicationProbe.Published);

        internal static AggregatePublicationProbeResult NotPublished { get; } =
            new(AggregatePublicationProbe.NotPublished);
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
