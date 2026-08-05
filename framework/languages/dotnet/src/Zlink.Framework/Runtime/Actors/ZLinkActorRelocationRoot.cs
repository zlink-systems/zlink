using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkActorRelocationRecoveryRecord(
    ZLinkRemoteActorJoinRequest Request,
    string TargetSpotId,
    byte[] TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong TargetSpotGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong OperationIdHigh,
    ulong OperationIdLow,
    string? ReplyContentType,
    byte[] Reply);

internal sealed record ZLinkLoadedActorRelocation(
    ZLinkRelocationEnvelope Envelope,
    ZLinkRelocationParticipantEnvelope Participant,
    ZLinkActorRelocationRecoveryRecord Recovery,
    IReadOnlyList<ZLinkActorHandoffFrame> AcceptedFrames);

internal static class ZLinkActorRelocationRoot
{
    internal static ZLinkLoadedActorRelocation Load(
        ZLinkRemoteActorJoinRequest wire,
        ZLinkRelocationEnvelope envelope)
    {
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(wire.ActorId);
        if (envelope.AggregateId != wire.RelocationAggregateId
            || envelope.AggregateGeneration
            != wire.RelocationAggregateGeneration
            || !envelope.InventoryDigest.Span.SequenceEqual(
                wire.RelocationInventoryDigest)
            || envelope.Participants.Count != 1
            || envelope.Participants[0].AuthorityKey != key)
            throw DataLost(
                $"Actor '{wire.ActorId}' relocation manifest does not match its durable root.");
        var participant = envelope.Participants[0];
        if (participant.ObjectKind != ZLinkPlacementObjectKind.Actor
            || participant.ObjectGeneration != wire.ActorGeneration
            || participant.AuthorityOwnerGeneration
            != wire.ActorAuthorityOwnerGeneration
            || participant.RecoveryPayload.IsEmpty)
            throw DataLost(
                $"Actor '{wire.ActorId}' relocation participant fences are invalid.");

        ZLinkActorRelocationRecoveryRecord recovery;
        ZLinkActorHandoffFrame[] frames;
        ZLinkCanonicalParticipantRecovery canonical;
        try
        {
            canonical =
                ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    participant.RecoveryPayload.Span);
            var sourceFence = ZLinkActorRelocationSourceFenceCodec.Decode(
                canonical.MembershipMutation.Span);
            recovery = ZLinkActorRemoteJoinRecoveryCodec.Decode(
                canonical.OperationRecovery.Span,
                sourceFence.LegacyRemoteJoinRecovery.Span);
            frames = participant.AcceptedJobs
                .OrderBy(static job => job.AcceptedSequence)
                .Select((job, index) =>
                    ZLinkCanonicalActorAcceptedJournal.Decode(
                        job.Payload.Span,
                        checked((long)index)).Frame)
                .ToArray();
        }
        catch (Exception error) when (error is InvalidDataException
                                      or EndOfStreamException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{wire.ActorId}' relocation recovery payload is malformed.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry,
                error);
        }
        if (canonical.AuthorityKey != participant.AuthorityKey
            || canonical.ObjectKind != participant.ObjectKind
            || canonical.ObjectGeneration != participant.ObjectGeneration
            || canonical.AuthorityOwnerGeneration
               != participant.AuthorityOwnerGeneration
            || !StringComparer.Ordinal.Equals(
                canonical.StableType,
                wire.ActorType))
            throw DataLost(
                $"Actor '{wire.ActorId}' canonical participant recovery fences are invalid.");
        var expectedInventoryDigest = ZLinkAggregateInventoryDigest.Compute(
        [
            new ZLinkAggregateRelocationParticipant(
                participant,
                canonical.ExpectedStoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                canonical.AuthorityPayload,
                ReadOnlyMemory<byte>.Empty)
        ]);
        if (!envelope.InventoryDigest.Span.SequenceEqual(
                expectedInventoryDigest))
            throw DataLost(
                $"Actor '{wire.ActorId}' relocation inventory mutation is invalid.");
        if (!string.Equals(
                recovery.Request.ActorId,
                wire.ActorId,
                StringComparison.Ordinal)
            || !string.Equals(
                recovery.Request.ActorType,
                wire.ActorType,
                StringComparison.Ordinal)
            || !string.Equals(
                recovery.Request.HandoffId,
                wire.HandoffId,
                StringComparison.Ordinal)
            || !string.Equals(
                recovery.TargetSpotId,
                ZLinkSpotId.Require(
                    recovery.TargetSpotId,
                    nameof(recovery.TargetSpotId)),
                StringComparison.Ordinal)
            || recovery.TargetNodeRid is not { Length: > 0 }
            || recovery.TargetNodeGeneration == 0
            || recovery.TargetSpotGeneration == 0
            || recovery.TargetAuthorityOwnerGeneration == 0
            || recovery.Request.TargetNodeRid is null
            || !recovery.TargetNodeRid.AsSpan().SequenceEqual(
                recovery.Request.TargetNodeRid)
            || recovery.TargetNodeGeneration
               != recovery.Request.TargetNodeGeneration
            || recovery.TargetSpotGeneration
               != recovery.Request.TargetSpotGeneration
            || recovery.TargetAuthorityOwnerGeneration
               != recovery.Request.TargetAuthorityOwnerGeneration)
            throw DataLost(
                $"Actor '{wire.ActorId}' relocation recovery identity is invalid.");
        var normalizedWire = wire with
        {
            RelocationReference =
                recovery.Request.RelocationReference,
            RelocationChecksumCrc32c =
                recovery.Request.RelocationChecksumCrc32c,
            RelocationAggregateId =
                recovery.Request.RelocationAggregateId,
            RelocationAggregateGeneration =
                recovery.Request.RelocationAggregateGeneration,
            RelocationInventoryDigest =
                recovery.Request.RelocationInventoryDigest,
            HandoffFrames = recovery.Request.HandoffFrames
        };
        if (!ZLinkActorHandoffRequestIdentity.Matches(
                recovery.Request,
                normalizedWire))
            throw DataLost(
                $"Actor '{wire.ActorId}' relocation wire changed its durable route or session fences.");
        return new ZLinkLoadedActorRelocation(
            envelope,
            participant,
            recovery,
            frames);
    }

    internal static ZLinkRelocationManifestReference Reference(
        ZLinkRemoteActorJoinRequest request) =>
        new(
            request.RelocationReference,
            request.RelocationChecksumCrc32c,
            request.RelocationAggregateId,
            request.RelocationAggregateGeneration,
            request.RelocationInventoryDigest);

    internal static ZLinkRemoteActorJoinRequest WithDurableFrames(
        ZLinkRemoteActorJoinRequest wire,
        ZLinkLoadedActorRelocation loaded) =>
        wire with
        {
            HandoffFrames = loaded.AcceptedFrames
        };

    private static ZLinkFrameworkException DataLost(string message) =>
        new(
            ZLinkFrameworkErrorKind.DataLost,
            message,
            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
}
