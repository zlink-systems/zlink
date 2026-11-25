using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Theory]
    [InlineData(-1)]
    [InlineData(1)]
    public async Task Aggregate_commit_reserves_final_completion_bytes(int spaceAfterReservation)
    {
        var request = CreateAggregateRelocationRequest(CreateEnvelope());
        var participants = request
            .Participants.Select(
                (participant, index) =>
                {
                    var entry = new ZLinkRelocationCompletionEntry(
                        participant.Envelope.AuthorityKey.Value,
                        $"0000000000000001000000000000000{index + 1}",
                        1,
                        "AQID",
                        new("requester", 1, "01", 1),
                        ZLinkRelocationCompletionDelivery.SourceLeaseExpired
                    );
                    var reservedBytes = new ZLinkRelocationCompletionRecord(
                        request.AggregateId,
                        [entry]
                    )
                        .Encode()
                        .Length;
                    return participant with
                    {
                        CompletionBudget = new(
                            request.AggregateId,
                            [entry with { Delivery = ZLinkRelocationCompletionDelivery.Pending }],
                            checked(
                                (ulong)(reservedBytes + (index == 0 ? spaceAfterReservation : 1))
                            )
                        ),
                    };
                }
            )
            .ToArray();
        request = request with { Participants = participants };
        var authority = new RecordingAuthorityStore();
        var blobs = new RecordingRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authority,
            blobs
        );

        if (spaceAfterReservation < 0)
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                coordinator.PublishAsync(request).AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, failure.Kind);
            Assert.Empty(authority.Events);
            Assert.Empty(blobs.Events);
            return;
        }

        await coordinator.PublishAsync(request);
        Assert.Equal(participants.Length, authority.PublishedCount);
        foreach (var participant in participants)
        {
            var read = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await authority.ReadAuthorityAsync(participant.Envelope.AuthorityKey)
            );
            Assert.Equal(
                ZLinkRelocationCompletionProgress.Start(1),
                read.Snapshot.Relocation?.Completion
            );
            Assert.Equal(request.TargetOwner.OwnerId, read.Snapshot.OwnerId);
        }
    }

    [Fact]
    public async Task Aggregate_cleanup_deletes_terminal_blob_only_after_unpublication()
    {
        var stage = CreateTargetStageForHeldJournal();
        var request = CreateAggregateRelocationRequest(stage.Envelope);
        var authorityStore = new RecordingAuthorityStore();
        var blobs = new RecordingRelocationStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            new AuditRuntimeFailureReporter(),
            authorityStore,
            blobs
        );
        await coordinator.PublishAsync(request);
        var key = request.Participants[0].Envelope.AuthorityKey;
        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await authorityStore.ReadAuthorityAsync(key))
            .Snapshot;
        var relocation = authority.Relocation!;
        var terminal = new ZLinkRelocationCompletionEntry(
            key.Value,
            "00000000000000010000000000000001",
            3,
            "AQID",
            new("source", 1, "01", 1),
            ZLinkRelocationCompletionDelivery.TerminalReceived
        );
        var payloads = new InMemoryRelocationStore();
        var (reference, sha) = await ZLinkRelocationTreeStore.PutCompletionAsync(
            payloads,
            new(stage.Envelope.AggregateId, [terminal]),
            Convert.FromHexString(relocation.SourceInventorySha256),
            default
        );
        var chunk = Assert.Single(payloads.Payloads.Keys.Where(key => key != reference));
        var participant = new ZLinkAggregateParticipantCompletion(
            new(stage.Envelope.AggregateId, [terminal]),
            new("target", 1, RoutingId.From("target"), 1, "source-version")
        )
        {
            LastObserved = authority with
            {
                Relocation = relocation with { Completion = new(1, 1, 0, reference, sha) },
            },
        };
        stage.Completion = new Dictionary<ZLinkAuthorityKey, ZLinkAggregateParticipantCompletion>
        {
            [key] = participant,
        };
        var outcome = ZLinkAggregateCommitResult.Stale;
        ValueTask ReleaseAsync() =>
            ZLinkSpotRetireTargetRuntime.ReleaseFinalRootAfterNormalizationAsync(
                stage,
                authorityAlreadyNormalized: false,
                () => ValueTask.FromResult(outcome),
                value => ZLinkRelocationTreeStore.DeleteTreeAsync(payloads, value, default)
            );

        await Assert.ThrowsAsync<ZLinkFrameworkException>(() => ReleaseAsync().AsTask());
        Assert.IsType<ZLinkRelocationReadResult.Found>(
            await payloads.GetRelocationAsync(reference)
        );
        Assert.IsType<ZLinkRelocationReadResult.Found>(await payloads.GetRelocationAsync(chunk));

        outcome = ZLinkAggregateCommitResult.Committed;
        await ReleaseAsync();
        Assert.IsType<ZLinkRelocationReadResult.Missing>(
            await payloads.GetRelocationAsync(reference)
        );
        Assert.IsType<ZLinkRelocationReadResult.Missing>(await payloads.GetRelocationAsync(chunk));
    }
}
