using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal static IReadOnlyList<ZLinkRelocationCompletionEntry> AcceptedAggregateReplies(
        TargetStage stage
    ) =>
        stage
            .Envelope.Participants.SelectMany(participant =>
                (
                    participant.AuthorityKey == stage.SpotParticipant.AuthorityKey
                        ? participant.AcceptedJobs.Concat(stage.SnapshotHeldRecords())
                        : participant.AcceptedJobs
                )
                    .Where(static job => job.CanonicalRequest is { ReplyRouteId: not 0 })
                    .Select(job =>
                    {
                        var request = job.CanonicalRequest!;
                        return CreateAcceptedRelocationReply(
                            participant.AuthorityKey,
                            new(request.OperationHigh, request.OperationLow),
                            request.ReplyRouteId,
                            new(
                                request.Source.OwnerId,
                                request.Source.OwnerLeaseGeneration,
                                RoutingId.FromHex(request.Source.NodeRid),
                                request.Source.NodeGeneration
                            )
                        );
                    })
            )
            .ToArray();

    private static ZLinkServiceWireCodec.ReplyRelayRecord AggregateCompletionRelay(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkRelocationQueuedJob job,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator
    )
    {
        var request = job.CanonicalRequest!;
        return new(
            new(request.OperationHigh, request.OperationLow),
            request.ReplyRouteId,
            new(stage.Envelope.CanonicalRelocationHigh, stage.Envelope.CanonicalRelocationLow),
            stage.TargetAttemptGeneration,
            coordinator,
            participant.CanonicalParticipantId,
            job.AcceptedSequence,
            0,
            0
        );
    }

    private void CaptureAggregateCompletion(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkRelocationQueuedJob job,
        IReadOnlyList<byte[]> payload
    )
    {
        var authority = stage.GetPublishedAuthority(participant.AuthorityKey);
        var writer = new ZLinkLocationOwnerToken(authority.OwnerId, authority.OwnerLeaseGeneration);
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            writer.OwnerId,
            checked((ulong)writer.LeaseGeneration),
            stage.Node.Node.RoutingId,
            stage.TargetNodeLifecycleGeneration,
            authority.StoreVersion
        );
        var relay = AggregateCompletionRelay(stage, participant, job, coordinator);
        var source = job.CanonicalRequest!.Source;
        var entry = CreateRelocationCompletionEntry(
            participant.AuthorityKey,
            relay,
            new(
                source.OwnerId,
                source.OwnerLeaseGeneration,
                RoutingId.FromHex(source.NodeRid),
                source.NodeGeneration
            ),
            payload
        );
        ScheduleRelocationReply(new(stage.Envelope.AggregateId, writer, entry), relay);
    }
}
