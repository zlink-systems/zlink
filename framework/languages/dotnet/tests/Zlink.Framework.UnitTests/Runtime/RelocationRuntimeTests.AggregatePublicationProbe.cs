using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Fact]
    public async Task AggregatePublicationProbeIsBoundedAndParallelForTenThousandParticipants()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
            BeforeRead = static token => Task.Delay(TimeSpan.FromMilliseconds(1), token),
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority,
            relocation
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
            new ZLinkLocationOwnerToken("aggregate-target", 17)
        );

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.PublishAsync(request)
        );

        Assert.InRange(authority.MaximumConcurrentReads, 2, 64);
        Assert.Contains(authority.Events, static item => item.Name == "abort");
        Assert.Contains(relocation.Events, static item => item.Name == "delete");
    }

    [Fact]
    public async Task AggregatePrepareResponseLossReconcilesPublishedAggregate()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { ThrowAfterPublishingAggregatePrepare = true };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var request = CreateAggregateRelocationRequest(CreateEnvelope());

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(request.AggregateId, published.Fence.AggregateId);
        Assert.Equal(request.Participants.Count, authority.PublishedCount);
    }

    [Fact]
    public async Task ConcurrentCommitDuringPrepareProbeReconcilesPublication()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { ThrowBeforeConcurrentAggregateCommit = true };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var request = CreateAggregateRelocationRequest(CreateEnvelope());

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(request.AggregateId, published.Fence.AggregateId);
        Assert.Equal(request.Participants.Count, authority.PublishedCount);
    }

    [Fact]
    public async Task AggregatePublicationProbeUsesTargetLifetimeCancellationAndPreservesUnknownRoot()
    {
        using var lifetime = new CancellationTokenSource();
        var releaseRead = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var readStarted = new TaskCompletionSource<Task>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var relocation = new RecordingRelocationStore();
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
        var coordinator = new ZLinkAggregateRelocationCoordinator(failures, authority, relocation);
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
        Assert.DoesNotContain(relocation.Events, static item => item.Name == "delete");
        Assert.NotEmpty(relocation.Payloads);
        Assert.Contains(failures.Failures, static error => error is OperationCanceledException);
    }

    [Fact]
    public async Task AggregatePublicationProbeMixedResultPreservesUnknownRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
            PublishFirstParticipantBeforePrepareConflict = true,
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var request = CreatePublicationProbeRequest();

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.PublishAsync(request)
        );

        Assert.Equal(1, authority.PublishedCount);
        Assert.DoesNotContain(authority.Events, static item => item.Name == "abort");
        Assert.DoesNotContain(relocation.Events, static item => item.Name == "delete");
        Assert.NotEmpty(relocation.Payloads);
    }

    [Fact]
    public async Task AggregatePrepareConflictDeletesUnpublishedRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict(),
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority,
            relocation
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
            new ZLinkLocationOwnerToken("aggregate-target", 17)
        );

        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.PublishAsync(request)
        );

        Assert.DoesNotContain(
            relocation.Payloads.Values,
            static payload => payload.Take(4).SequenceEqual("ZLTM"u8.ToArray())
        );
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
