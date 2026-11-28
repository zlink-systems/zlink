using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Fact]
    public async Task AggregatePublicationProbeIsBoundedAndParallelForTenThousandParticipants()
    {
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
            BeforeRead = static token => Task.Delay(TimeSpan.FromMilliseconds(1), token),
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority
        );
        var envelope = CreateLargeEnvelope(10_000);
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            1,
            envelope
                .Participants.Select(participant => new ZLinkAggregateRelocationParticipant(
                    participant,
                    $"v-{participant.AuthorityKey.Value}",
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    ReadOnlyMemory<byte>.Empty,
                    ReadOnlyMemory<byte>.Empty
                ))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(10_000, 0, null),
            new ZLinkLocationOwnerToken("aggregate-target", 17),
            AggregateSourceFence()
        );

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.PublishAsync(request)
        );

        Assert.InRange(authority.MaximumConcurrentReads, 2, 64);
        Assert.Contains(authority.Events, static item => item.Name == "abort");
    }

    [Fact]
    public async Task AggregatePrepareResponseLossReconcilesPublishedAggregate()
    {
        var authority = new RecordingAuthorityStore { ThrowAfterPublishingAggregatePrepare = true };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority
        );
        var request = CreateAggregateRelocationRequest(CreateEnvelope());

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(request.AggregateId, published.Fence.AggregateId);
        Assert.Equal(request.Participants.Count, authority.PublishedCount);
    }

    [Fact]
    public async Task ConcurrentCommitDuringPrepareProbeReconcilesPublication()
    {
        var authority = new RecordingAuthorityStore { ThrowBeforeConcurrentAggregateCommit = true };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority
        );
        var request = CreateAggregateRelocationRequest(CreateEnvelope());

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(request.AggregateId, published.Fence.AggregateId);
        Assert.Equal(request.Participants.Count, authority.PublishedCount);
    }

    [Fact]
    public async Task AggregatePublicationProbeUsesTargetLifetimeCancellation()
    {
        using var lifetime = new CancellationTokenSource();
        var releaseRead = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var readStarted = new TaskCompletionSource<Task>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
            BeforeRead = token =>
            {
                var read = releaseRead.Task.WaitAsync(token);
                readStarted.TrySetResult(read);
                return read;
            },
        };
        var failures = new AuditRuntimeFailureReporter();
        var coordinator = new ZLinkAggregateRelocationCoordinator(failures, authority);
        var request = CreatePublicationProbeRequest();

        var publication = coordinator.PublishAsync(request, lifetime.Token).AsTask();
        Assert.Same(readStarted.Task, await Task.WhenAny(readStarted.Task, publication));
        var read = await readStarted.Task;
        lifetime.Cancel();
        var readWasCanceled = read.IsCanceled;
        // Release any read that did not receive the target lifetime cancellation.
        releaseRead.SetResult();

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(() => publication);
        Assert.True(
            readWasCanceled,
            "The target lifetime did not cancel the pending authority read."
        );

        Assert.DoesNotContain(authority.Events, static item => item.Name == "abort");
        Assert.Contains(failures.Failures, static error => error is OperationCanceledException);
    }

    [Fact]
    public async Task AggregatePublicationProbeMixedResultDoesNotAbort()
    {
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
            PublishFirstParticipantBeforePrepareConflict = true,
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority
        );
        var request = CreatePublicationProbeRequest();

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.PublishAsync(request)
        );

        Assert.Equal(1, authority.PublishedCount);
        Assert.DoesNotContain(authority.Events, static item => item.Name == "abort");
    }

    [Fact]
    public async Task AggregatePrepareConflictAbortsUnpublishedAggregate()
    {
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority
        );
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            1,
            envelope
                .Participants.Select(participant => new ZLinkAggregateRelocationParticipant(
                    participant,
                    "v1",
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    ReadOnlyMemory<byte>.Empty,
                    ReadOnlyMemory<byte>.Empty
                ))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(ZLinkPlacementObjectKind.UserSpot, "room", 1)
            ),
            new ZLinkLocationOwnerToken("aggregate-target", 17),
            AggregateSourceFence()
        );

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.PublishAsync(request)
        );

        Assert.Contains(authority.Events, static item => item.Name == "abort");
    }

    private static ZLinkAggregateRelocationRequest CreatePublicationProbeRequest() =>
        CreateAggregateRelocationRequest(CreateEnvelope()) with
        {
            Capacity = new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(ZLinkPlacementObjectKind.UserSpot, "room", 1)
            ),
        };
}
