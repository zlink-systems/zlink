using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal void ReserveAggregateCompletion(TargetStage stage, ZLinkLocationOwnerToken owner)
    {
        var budgets = new Dictionary<ZLinkAuthorityKey, ZLinkAggregateParticipantCompletion>();
        foreach (var participant in stage.Envelope.Participants)
        {
            var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                stage.Node.Node.RoutingId,
                stage.TargetNodeLifecycleGeneration,
                ZLinkCanonicalParticipantRecoveryCodec
                    .Decode(participant.RecoveryPayload.Span)
                    .ExpectedStoreVersion
            );
            budgets.Add(
                participant.AuthorityKey,
                new(
                    CreateAggregateCompletionBudget(
                        stage,
                        participant,
                        coordinator,
                        participant.AcceptedJobs
                    ),
                    coordinator
                )
            );
        }
        stage.Completion = budgets;
    }

    internal ZLinkRelocationCompletionBudget ReserveSpotCompletion(TargetStage stage)
    {
        var participant = stage.SpotParticipant;
        var completion = stage.Completion[participant.AuthorityKey];
        var budget = CreateAggregateCompletionBudget(
            stage,
            participant,
            completion.Coordinator,
            participant.AcceptedJobs.Concat(stage.SnapshotHeldRecords())
        );
        stage.Completion[participant.AuthorityKey] = new(budget, completion.Coordinator);
        return budget;
    }

    private static ZLinkRelocationCompletionBudget CreateAggregateCompletionBudget(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator,
        IEnumerable<ZLinkRelocationQueuedJob> jobs
    ) =>
        new(
            stage.Envelope.AggregateId,
            jobs.Where(static job => job.CanonicalRequest is { ReplyRouteId: not 0 })
                .Select(job =>
                    AggregateCompletionEntry(stage, participant, job, coordinator, payload: null)
                )
        );

    private static ZLinkRelocationCompletionEntry AggregateCompletionEntry(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkRelocationQueuedJob job,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator,
        IReadOnlyList<byte[]>? payload
    )
    {
        var request = job.CanonicalRequest!;
        var source = request.Source;
        return CreateRelocationCompletionEntry(
            participant.AuthorityKey,
            new ZLinkServiceWireCodec.ReplyRelayRecord(
                new(request.OperationHigh, request.OperationLow),
                request.ReplyRouteId,
                new(stage.Envelope.CanonicalRelocationHigh, stage.Envelope.CanonicalRelocationLow),
                stage.TargetAttemptGeneration,
                coordinator,
                participant.CanonicalParticipantId,
                job.AcceptedSequence,
                payload is null ? (uint)RequestResult.InternalError : 0,
                payload is null ? ServiceWireConstants.FrameworkErrorCode.RequestFailed : 0
            ),
            new(
                source.OwnerId,
                source.OwnerLeaseGeneration,
                RoutingId.FromHex(source.NodeRid),
                source.NodeGeneration
            ),
            payload
        );
    }

    private static void CaptureAggregateCompletion(
        TargetStage stage,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkRelocationQueuedJob job,
        IReadOnlyList<byte[]> payload
    )
    {
        var completion = stage.Completion[participant.AuthorityKey];
        stage.RecordCompletion(
            completion,
            job.AcceptedSequence,
            AggregateCompletionEntry(stage, participant, job, completion.Coordinator, payload)
        );
    }

    internal async ValueTask RetainAggregateCompletionAsync(
        TargetStage stage,
        CancellationToken cancellationToken
    )
    {
        if (stage.Completion.Count == 0)
            return;
        var store = CreateRelocationCompletionStore();
        foreach (var participant in stage.Completion.Values)
            if (participant.LastObserved?.Relocation is { } latest)
                await store.RetainIfPendingAsync(latest, cancellationToken).ConfigureAwait(false);
    }

    internal async ValueTask<bool> CompleteAggregateRequestsAsync(
        TargetStage stage,
        CancellationToken cancellationToken
    )
    {
        var ready = new List<(ZLinkAuthorityKey Key, ZLinkAuthoritySnapshot Authority)>(
            stage.Completion.Count
        );
        foreach (var (key, completion) in stage.Completion)
        {
            var store = CreateRelocationCompletionStore(completion.Budget);
            var expected = completion.LastObserved ?? stage.GetPublishedAuthority(key);
            ZLinkAuthoritySnapshot current;
            try
            {
                current = await store
                    .ReadCurrentAsync(key, expected, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception error)
                when (ZLinkLocationStoreFailure.IsUnavailable(error, cancellationToken))
            {
                await store
                    .RetainIfPendingAsync(expected.Relocation!, cancellationToken)
                    .ConfigureAwait(false);
                throw;
            }
            completion.LastObserved = current;
            var progress = ZLinkRelocationCompletionStore.RequireAcceptedRequests(
                current.Relocation!,
                completion.Budget.AcceptedRequests
            );
            if (progress.CompletedResults != progress.AcceptedRequests)
            {
                if ((ulong)completion.Results.Count != progress.AcceptedRequests)
                    throw new ZLinkRelocationDataLostException(
                        "Aggregate completion does not match the accepted replay."
                    );
                foreach (var entry in completion.Results.Values)
                {
                    (current, _) = await store
                        .AppendAsync(key, current, entry, cancellationToken)
                        .ConfigureAwait(false);
                    completion.LastObserved = current;
                }
            }
            completion.Results.Clear();

            current = await store
                .RedeliverPendingAsync(
                    key,
                    current,
                    TryDeliverStoredRelocationReplyAsync,
                    cancellationToken
                )
                .ConfigureAwait(false);
            completion.LastObserved = current;
            if (current.Relocation!.Completion is { IsReady: true })
                ready.Add((key, current));
        }
        if (ready.Count != stage.Completion.Count)
            return false;
        if (Volatile.Read(ref stage.SessionRoutesConverged) == 0)
            return false;
        if (ready.Count == 0)
            return true;
        var completionStore = CreateRelocationCompletionStore();
        foreach (var (key, authority) in ready)
            await completionStore
                .MarkCompletedAsync(key, authority, cancellationToken)
                .ConfigureAwait(false);
        foreach (var actor in stage.ActorStates)
            if (
                actor.RelocationCompletionCandidate is { } candidate
                && candidate.RelocationId == stage.Envelope.AggregateId
            )
                actor.ClearRelocationCompletionCandidate(candidate);
        return true;
    }
}

internal sealed record ZLinkAggregateParticipantCompletion(
    ZLinkRelocationCompletionBudget Budget,
    ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator
)
{
    internal SortedDictionary<ulong, ZLinkRelocationCompletionEntry> Results { get; } = new();

    // The last verified pointer can only renew retention during an uncertain read.
    internal ZLinkAuthoritySnapshot? LastObserved { get; set; }
}
