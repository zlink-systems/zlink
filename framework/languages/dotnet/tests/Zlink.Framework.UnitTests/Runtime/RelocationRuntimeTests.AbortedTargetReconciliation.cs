using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Fact]
    public async Task Queued_reconciliation_rejects_target_aborted_after_commit()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            new ZLinkFrameworkRegistration()
        );
        var (stage, candidate) = CreateCanonicalPublishedReconciliationFixture();
        stage.RememberAuthorityPublication(
            candidate.Authorities.Select(entry => KeyValuePair.Create(entry.Key, entry.Snapshot))
        );
        stage.AuthorityPublished = 1;
        // Make an unexpected entry into finalization fail before it dispatches against the fixture.
        stage.AdmissionDrainTask = Task.FromCanceled(new CancellationToken(canceled: true));
        var fence = new ZLinkAggregateFence(
            stage.Envelope.AggregateId,
            stage.Envelope.AggregateGeneration
        );
        Assert.True(target.TryTrackStage(fence, stage));

        Task<bool> reconciliation;
        await stage.PublishGate.WaitAsync();
        try
        {
            reconciliation = target
                .PublishInboundAsync(fence, stage.SourceNodeRid, CancellationToken.None)
                .AsTask();
            Assert.False(reconciliation.IsCompleted);

            // The in-flight publication discards the local stage after an initialization failure.
            Assert.True(
                await stage.RunAbortCleanupAsync(
                    () =>
                    {
                        target.CompleteStage(stage, TargetStageTerminalOutcome.Aborted);
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
            );
        }
        finally
        {
            stage.PublishGate.Release();
        }

        Assert.Equal(TargetStageAbortState.Aborted, stage.AbortState);
        Assert.Equal(0, target.ActiveStageCount);
        Assert.False(await reconciliation.WaitAsync(TimeSpan.FromSeconds(5)));
    }
}
