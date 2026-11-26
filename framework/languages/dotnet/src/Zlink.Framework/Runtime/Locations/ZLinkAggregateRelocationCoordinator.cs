using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkAggregateRelocationParticipant(
    ZLinkRelocationParticipantEnvelope Envelope,
    string ExpectedStoreVersion,
    ZLinkAuthorityGenerationTransition OwnerTransition,
    ReadOnlyMemory<byte> ApplicationAuthorityPayload,
    ReadOnlyMemory<byte> MembershipMutation
)
{
    internal string? CapturedStoreVersion { get; init; }
    internal string CompareExchangeStoreVersion => CapturedStoreVersion ?? ExpectedStoreVersion;
}

internal sealed record ZLinkAggregateRelocationRequest(
    Guid AggregateId,
    ulong AggregateGeneration,
    ulong TargetAttemptGeneration,
    IReadOnlyList<ZLinkAggregateRelocationParticipant> Participants,
    ZLinkMeshNodeDescriptorKey TargetDescriptor,
    ulong TargetDescriptorLifecycleGeneration,
    ZLinkCapacityVector Capacity,
    ZLinkLocationOwnerToken TargetOwner,
    ZLinkAuthorityRelocation.CoordinatorFence Source,
    ZLinkRelocationEnvelope? CanonicalEnvelope = null,
    ZLinkAuthorityKey? RepresentativeKey = null
);

internal sealed record ZLinkAggregateRelocationPublished(
    ZLinkAggregateFence Fence,
    ZLinkRelocationEnvelope Envelope
);

internal sealed class ZLinkAuthorityGenerationExhaustedException(string operation)
    : InvalidOperationException($"Authority generation was exhausted while {operation}.");

internal sealed class ZLinkAggregateRelocationCoordinator(
    IZLinkRuntimeFailureReporter failureReporter,
    IZLinkLocationRepository authorityStore
)
{
    internal sealed record PublicationState(
        ZLinkAggregateFence Fence,
        ZLinkRelocationEnvelope Envelope,
        IReadOnlyList<ZLinkAggregateRelocationParticipant> Participants,
        ZLinkAuthorityRelocation AuthorityRelocation,
        bool HasRepresentative
    );

    private const int MaxPublicationProbeConcurrency = 64;

    internal async ValueTask<ZLinkAggregateRelocationPublished> PublishAsync(
        ZLinkAggregateRelocationRequest request,
        CancellationToken cancellationToken = default
    )
    {
        var publication = await PreparePublicationAsync(request, cancellationToken)
            .ConfigureAwait(false);
        return await CommitPublicationAsync(publication, cancellationToken).ConfigureAwait(false);
    }

    internal async ValueTask<PublicationState> PreparePublicationAsync(
        ZLinkAggregateRelocationRequest request,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(request);
        Validate(request);
        cancellationToken.ThrowIfCancellationRequested();

        var inventoryDigest = request.CanonicalEnvelope is not null
            ? request.CanonicalEnvelope.InventoryDigest.ToArray()
            : ZLinkAggregateInventoryDigest.Compute(request.Participants);
        var envelope =
            request.CanonicalEnvelope
            ?? new ZLinkRelocationEnvelope(
                request.AggregateId,
                request.AggregateGeneration,
                inventoryDigest,
                request.Participants.Select(static participant => participant.Envelope).ToArray()
            );
        var relocation = new ZLinkAuthorityRelocation(
            envelope.AggregateId,
            ServiceWireCodec.RelocationPhase.Committed,
            request.Source.Identity,
            new(
                request.TargetDescriptor.Rid.ToHex(),
                request.TargetDescriptorLifecycleGeneration,
                request.TargetOwner.OwnerId,
                checked((ulong)request.TargetOwner.LeaseGeneration),
                request.TargetAttemptGeneration
            ),
            request.Source,
            Convert.ToHexString(inventoryDigest).ToLowerInvariant(),
            checked((ulong)envelope.CanonicalApplicationVersion)
        );
        var fence = new ZLinkAggregateFence(request.AggregateId, request.AggregateGeneration);
        try
        {
            var publicationParticipants = ZLinkAggregateInventoryDigest.Authorities(
                request.Participants
            );
            var prepare = await authorityStore
                .PrepareAggregateAsync(
                    new ZLinkAggregatePrepareRequest(
                        request.AggregateId,
                        request.AggregateGeneration,
                        publicationParticipants,
                        inventoryDigest,
                        request.TargetDescriptor,
                        request.TargetDescriptorLifecycleGeneration,
                        request.Capacity,
                        request.TargetOwner
                    )
                    {
                        Relocation = relocation,
                        RepresentativeKey = request.RepresentativeKey,
                    },
                    cancellationToken
                )
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
                        "preparing an aggregate relocation"
                    );
                default:
                    throw new ZLinkRelocationTargetSettledException(
                        "The aggregate relocation prepare was rejected."
                    );
            }

            return new PublicationState(
                fence,
                envelope,
                request.Participants,
                relocation,
                request.RepresentativeKey is not null
            );
        }
        catch
        {
            var publication = await ProbePublicationAsync(
                    request.Participants,
                    relocation,
                    request.RepresentativeKey is not null,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (publication.State == AggregatePublicationProbe.Published)
                return new PublicationState(
                    fence,
                    envelope,
                    request.Participants,
                    relocation,
                    request.RepresentativeKey is not null
                );
            if (
                request.RepresentativeKey is not null
                || publication.State == AggregatePublicationProbe.Unknown
            )
                throw;

            try
            {
                await authorityStore
                    .AbortAggregateAsync(fence, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failureReporter.ReportRuntimeTaskException(
                    nameof(PreparePublicationAsync),
                    exception
                );
            }
            throw;
        }
    }

    internal async ValueTask<ZLinkAggregateRelocationPublished> CommitPublicationAsync(
        PublicationState publication,
        CancellationToken cancellationToken = default
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            var commit = publication.HasRepresentative
                ? await authorityStore
                    .CommitAggregateAttemptAsync(
                        publication.Fence,
                        publication.AuthorityRelocation.Target!.AttemptGeneration,
                        cancellationToken
                    )
                    .ConfigureAwait(false)
                : await authorityStore
                    .CommitAggregateAsync(publication.Fence, cancellationToken)
                    .ConfigureAwait(false);
            if (commit == ZLinkAggregateCommitResult.GenerationExhausted)
                throw new ZLinkAuthorityGenerationExhaustedException(
                    "committing an aggregate relocation"
                );
            if (
                commit
                is not (
                    ZLinkAggregateCommitResult.Committed
                    or ZLinkAggregateCommitResult.AlreadyCommitted
                )
            )
                throw new ZLinkRelocationTargetSettledException(
                    $"The aggregate relocation commit failed with '{commit}'."
                );
        }
        catch
        {
            if (
                (
                    await ProbePublicationAsync(
                            publication.Participants,
                            publication.AuthorityRelocation,
                            publication.HasRepresentative,
                            cancellationToken
                        )
                        .ConfigureAwait(false)
                ).State != AggregatePublicationProbe.Published
            )
                throw;
        }

        return new ZLinkAggregateRelocationPublished(publication.Fence, publication.Envelope);
    }

    internal static ReadOnlyMemory<byte> ProjectParticipantPayload(
        ZLinkPlacementObjectKind kind,
        ReadOnlyMemory<byte> sourcePayload,
        ZLinkMeshNodeDescriptorKey target,
        ulong targetNodeGeneration,
        ZLinkLocationOwnerToken targetOwner,
        Guid relocationId,
        ZLinkActorRelocationAuthorityPhase actorPhase = ZLinkActorRelocationAuthorityPhase.Steady
    )
    {
        var ownerGeneration = checked((ulong)targetOwner.LeaseGeneration);
        if (
            kind == ZLinkPlacementObjectKind.UserSpot
            && ZLinkUserSpotAuthorityPayloadCodec.TryDecode(sourcePayload.Span, out var user)
        )
            return ZLinkUserSpotAuthorityPayloadCodec.Encode(
                user with
                {
                    State = ZLinkUserSpotAuthorityState.Ready,
                    OwnerId = targetOwner.OwnerId,
                    OwnerLeaseGeneration = ownerGeneration,
                    NodeRid = target.Rid,
                    NodeGeneration = targetNodeGeneration,
                }
            );
        if (
            kind == ZLinkPlacementObjectKind.InstanceSpot
            && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                sourcePayload.Span,
                out var instance
            )
        )
            return ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                instance with
                {
                    State = ZLinkInstanceSpotAuthorityState.Ready,
                    OwnerId = targetOwner.OwnerId,
                    OwnerLeaseGeneration = ownerGeneration,
                    NodeRid = target.Rid,
                    NodeGeneration = targetNodeGeneration,
                }
            );
        if (
            kind == ZLinkPlacementObjectKind.Actor
            && ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                sourcePayload.Span,
                out var relocation
            )
            && relocation.RelocationId == relocationId
            && relocation.Phase == ZLinkActorRelocationAuthorityPhase.Activated
            && ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                relocation.ApplicationPayload.Span,
                out var actor
            )
        )
            return ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                relocation with
                {
                    Phase = actorPhase,
                    ApplicationPayload = ZLinkActorAuthorityPayloadCodec.Encode(
                        actor with
                        {
                            State = ZLinkActorAuthorityState.Ready,
                            OwnerId = targetOwner.OwnerId,
                            OwnerLeaseGeneration = ownerGeneration,
                            NodeRid = target.Rid,
                            NodeGeneration = targetNodeGeneration,
                        }
                    ),
                }
            );
        if (
            kind == ZLinkPlacementObjectKind.Actor
            && ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                sourcePayload.Span,
                out var canonicalActor
            )
        )
            return ZLinkActorAuthorityPayloadCodec.Encode(
                canonicalActor with
                {
                    State = ZLinkActorAuthorityState.Ready,
                    OwnerId = targetOwner.OwnerId,
                    OwnerLeaseGeneration = ownerGeneration,
                    NodeRid = target.Rid,
                    NodeGeneration = targetNodeGeneration,
                }
            );
        throw new ZLinkRelocationDataLostException(
            $"Relocation participant kind '{kind}' has an invalid authority payload."
        );
    }

    internal static bool IsExactNormalizedTargetAuthority(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkRelocationParticipantEnvelope participant,
        Guid aggregateId,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        ulong targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        bool allowCurrentAuthorityGeneration = false
    )
    {
        if (
            snapshot.Relocation is not null
            || ZLinkRelocationAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out _)
            || snapshot.Allocation.ObjectKind != participant.ObjectKind
            || snapshot.Allocation.Descriptor != targetDescriptor
            || snapshot.Allocation.DescriptorLifecycleGeneration
                != targetDescriptorLifecycleGeneration
            || snapshot.OwnerId != targetOwner.OwnerId
            || snapshot.OwnerLeaseGeneration != targetOwner.LeaseGeneration
            || snapshot.ObjectGeneration != participant.ObjectGeneration
            || participant.AuthorityOwnerGeneration is 0 or > long.MaxValue
            || (
                allowCurrentAuthorityGeneration
                    ? snapshot.AuthorityOwnerGeneration < participant.AuthorityOwnerGeneration
                    : snapshot.AuthorityOwnerGeneration <= participant.AuthorityOwnerGeneration
            )
        )
            return false;

        ZLinkCanonicalParticipantRecovery recovery;
        try
        {
            recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                participant.RecoveryPayload.Span
            );
        }
        catch (Exception error)
            when (error is InvalidDataException or ArgumentException or OverflowException)
        {
            return false;
        }
        if (
            recovery.AuthorityKey != participant.AuthorityKey
            || recovery.ObjectKind != participant.ObjectKind
            || recovery.ObjectGeneration != participant.ObjectGeneration
            || recovery.AuthorityOwnerGeneration != participant.AuthorityOwnerGeneration
            || snapshot.Allocation.StableType != recovery.StableType
        )
            return false;

        var expected = ProjectParticipantPayload(
            participant.ObjectKind,
            recovery.AuthorityPayload,
            targetDescriptor,
            targetDescriptorLifecycleGeneration,
            targetOwner,
            aggregateId
        );
        return snapshot.Payload.Span.SequenceEqual(expected.Span);
    }

    private async ValueTask<AggregatePublicationProbeResult> ProbePublicationAsync(
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants,
        ZLinkAuthorityRelocation relocation,
        bool projectPayload,
        CancellationToken cancellationToken
    )
    {
        var results = new AggregatePublicationProbe[participants.Count];
        try
        {
            await Parallel
                .ForEachAsync(
                    Enumerable.Range(0, participants.Count),
                    new ParallelOptions
                    {
                        MaxDegreeOfParallelism = MaxPublicationProbeConcurrency,
                        CancellationToken = cancellationToken,
                    },
                    async (index, token) =>
                    {
                        var participant = participants[index];
                        var read = await authorityStore
                            .ReadAuthorityAsync(participant.Envelope.AuthorityKey, token)
                            .ConfigureAwait(false);
                        if (
                            read is ZLinkAuthorityReadResult.Found found
                            && ZLinkAuthorityRelocation.MatchesCommittedTarget(
                                found.Snapshot,
                                relocation.RelocationId,
                                relocation.Target!.AttemptGeneration
                            )
                            && found.Snapshot.Relocation!.Target == relocation.Target
                            && found.Snapshot.Relocation.SourceInventorySha256
                                == relocation.SourceInventorySha256
                            && found.Snapshot.Payload.Span.SequenceEqual(
                                (
                                    projectPayload
                                        ? ProjectParticipantPayload(
                                            participant.Envelope.ObjectKind,
                                            participant.ApplicationAuthorityPayload,
                                            found.Snapshot.Allocation.Descriptor,
                                            found.Snapshot.Allocation.DescriptorLifecycleGeneration,
                                            new(
                                                found.Snapshot.OwnerId,
                                                found.Snapshot.OwnerLeaseGeneration
                                            ),
                                            relocation.RelocationId
                                        )
                                        : participant.ApplicationAuthorityPayload
                                ).Span
                            )
                        )
                        {
                            results[index] = AggregatePublicationProbe.Published;
                        }
                        else
                        {
                            results[index] = AggregatePublicationProbe.NotPublished;
                        }
                    }
                )
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            failureReporter.ReportRuntimeTaskException(nameof(ProbePublicationAsync), exception);
            return AggregatePublicationProbeResult.Unknown;
        }

        if (results.All(static result => result == AggregatePublicationProbe.Published))
            return AggregatePublicationProbeResult.Published;
        if (results.All(static result => result == AggregatePublicationProbe.NotPublished))
            return AggregatePublicationProbeResult.NotPublished;
        return AggregatePublicationProbeResult.Unknown;
    }

    private enum AggregatePublicationProbe
    {
        Unknown = 0,
        Published = 1,
        NotPublished = 2,
    }

    private sealed record AggregatePublicationProbeResult(AggregatePublicationProbe State)
    {
        internal static AggregatePublicationProbeResult Unknown { get; } =
            new(AggregatePublicationProbe.Unknown);

        internal static AggregatePublicationProbeResult Published { get; } =
            new(AggregatePublicationProbe.Published);

        internal static AggregatePublicationProbeResult NotPublished { get; } =
            new(AggregatePublicationProbe.NotPublished);
    }

    private static void Validate(ZLinkAggregateRelocationRequest request)
    {
        if (request.AggregateId == Guid.Empty)
            throw new ArgumentException("The aggregate id must not be empty.", nameof(request));
        if (
            request.AggregateGeneration is 0 or >= long.MaxValue
            || request.TargetAttemptGeneration is 0 or > long.MaxValue
        )
            throw new ArgumentOutOfRangeException(nameof(request));
        if (request.TargetOwner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(request));
        if (request.Participants.Count < 1)
            throw new ArgumentOutOfRangeException(nameof(request));
        var keys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var participant in request.Participants)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(participant.ExpectedStoreVersion);
            if (!keys.Add(participant.Envelope.AuthorityKey.Value))
                throw new ArgumentException(
                    "Aggregate participant keys must be unique.",
                    nameof(request)
                );
        }
    }
}

internal static class ZLinkAggregateInventoryDigest
{
    internal static ZLinkAggregateParticipant[] Authorities(
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants
    ) =>
        participants
            .Select(static participant => new ZLinkAggregateParticipant(
                participant.Envelope.AuthorityKey,
                participant.ExpectedStoreVersion,
                participant.OwnerTransition,
                participant.ApplicationAuthorityPayload,
                ZLinkAggregateInventory.MembershipBody(participant.MembershipMutation)
            )
            {
                CapturedStoreVersion = participant.CapturedStoreVersion,
            })
            .ToArray();

    internal static byte[] Compute(
        IReadOnlyList<ZLinkAggregateRelocationParticipant> participants
    ) =>
        Convert.FromHexString(
            ZLinkAggregateInventory.Plan(Authorities(participants)).Root.DigestSha256
        );
}
