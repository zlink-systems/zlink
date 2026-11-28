using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Fact]
    public async Task NormalizationResponseLossRetriesFinalRootDeletion()
    {
        var stage = CreateTargetStageForHeldJournal();
        var completion = TrackVerifiedCompletionRoot(stage, "completed-root");
        var authorityNormalized = false;
        var deleted = new List<string>();

        await Assert.ThrowsAsync<IOException>(() =>
            ZLinkSpotRetireTargetRuntime
                .ReleaseFinalRootAfterNormalizationAsync(
                    stage,
                    authorityAlreadyNormalized: false,
                    () =>
                    {
                        authorityNormalized = true;
                        return ValueTask.FromException<ZLinkAggregateCommitResult>(
                            new IOException("normalization response lost")
                        );
                    },
                    reference =>
                    {
                        deleted.Add(reference);
                        return ValueTask.CompletedTask;
                    }
                )
                .AsTask()
        );

        Assert.True(authorityNormalized);
        Assert.False(stage.Terminal.Task.IsCompleted);
        Assert.Empty(deleted);

        await ZLinkSpotRetireTargetRuntime.ReleaseFinalRootAfterNormalizationAsync(
            stage,
            authorityAlreadyNormalized: true,
            commitNormalization: null,
            reference =>
            {
                Assert.True(stage.Terminal.Task.IsCompletedSuccessfully);
                deleted.Add(reference);
                return ValueTask.CompletedTask;
            }
        );

        Assert.Equal(["completed-root"], deleted);
        Assert.Equal("completed-root", completion.LastObserved?.Relocation?.Completion?.Reference);
    }

    [Fact]
    public async Task NormalizationDeletesOnlyTheLatestVerifiedSuccessorRoot()
    {
        var stage = CreateTargetStageForHeldJournal();
        var completion = TrackVerifiedCompletionRoot(stage, "completed-root-1");
        var observed = completion.LastObserved!;
        var relocation = observed.Relocation!;
        completion.LastObserved = observed with
        {
            Relocation = relocation with
            {
                Completion = relocation.Completion! with { Reference = "completed-root-2" },
            },
        };
        var deleted = new List<string>();

        await ZLinkSpotRetireTargetRuntime.ReleaseFinalRootAfterNormalizationAsync(
            stage,
            authorityAlreadyNormalized: true,
            commitNormalization: null,
            reference =>
            {
                deleted.Add(reference);
                return ValueTask.CompletedTask;
            }
        );

        Assert.Equal(["completed-root-2"], deleted);
    }

    [Fact]
    public async Task NormalizationGenerationExhaustionPreservesFinalRoot()
    {
        var stage = CreateTargetStageForHeldJournal();
        var completion = TrackVerifiedCompletionRoot(stage, "completed-root");
        var deleted = false;

        await Assert.ThrowsAsync<ZLinkAuthorityGenerationExhaustedException>(() =>
            ZLinkSpotRetireTargetRuntime
                .ReleaseFinalRootAfterNormalizationAsync(
                    stage,
                    authorityAlreadyNormalized: false,
                    static () =>
                        ValueTask.FromResult(ZLinkAggregateCommitResult.GenerationExhausted),
                    _ =>
                    {
                        deleted = true;
                        return ValueTask.CompletedTask;
                    }
                )
                .AsTask()
        );

        Assert.False(deleted);
        Assert.Equal("completed-root", completion.LastObserved?.Relocation?.Completion?.Reference);
    }

    private static ZLinkAggregateParticipantCompletion TrackVerifiedCompletionRoot(
        TargetStage stage,
        string reference
    )
    {
        var source = AggregateSourceFence();
        var completion = new ZLinkAggregateParticipantCompletion(null!, default)
        {
            LastObserved = new ZLinkAuthoritySnapshot(
                "v-completed",
                ReadOnlyMemory<byte>.Empty,
                1,
                2,
                "target-owner",
                1,
                null!,
                null,
                DateTimeOffset.UnixEpoch
            )
            {
                Relocation = new ZLinkAuthorityRelocation(
                    stage.Envelope.AggregateId,
                    ServiceWireCodec.RelocationPhase.Committed,
                    source.Identity,
                    null,
                    source,
                    string.Empty,
                    1,
                    new(1, 1, 0, reference, null)
                ),
            },
        };
        stage.Completion.Add(stage.SpotParticipant.AuthorityKey, completion);
        return completion;
    }
}
