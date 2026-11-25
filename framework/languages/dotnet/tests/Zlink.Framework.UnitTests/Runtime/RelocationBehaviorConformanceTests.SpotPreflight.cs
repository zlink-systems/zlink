using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData(false, 0)]
    [InlineData(true, 1)]
    public async Task Spot_preflight_reads_only_finishing_target_authority(
        bool hasFinishingTarget,
        int expectedReads
    )
    {
        var spotIds = Enumerable
            .Range(0, hasFinishingTarget ? 1 : 3)
            .Select(_ => $"preflight-spot-{Guid.NewGuid():N}")
            .ToArray();
        var store = new PreflightAuthorityCountingStore(
            new ZLinkInMemoryProviderLocationStore(),
            spotIds.Select(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey)
        );
        var roots = new SynchronizedRelocationStore();
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseTargetLifecycle.TrySetResult();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        var spots = source.Services.GetRequiredService<IZLinkSpotManager>();
        var timeout = TimeSpan.FromSeconds(10);
        foreach (var spotId in spotIds)
            _ = await spots
                .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Timeout(timeout)
                .Async();

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        await WaitUntilAsync(() =>
            source
                .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                .Node.Status()
                .ActivePeerCount == 1
            && target
                .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                .Node.Status()
                .ActivePeerCount == 1
        );
        var normalization = SpotWideNormalizationGate.Install(
            target.Runtime.Registration.Locations
        );
        Task<ZLinkFrameworkRelocationResult>? relocation = null;
        try
        {
            if (hasFinishingTarget)
            {
                relocation = source
                    .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                    .RelocateAsync(
                        new ZLinkFrameworkRelocationOptions
                        {
                            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                            Deadline = TimeSpan.FromSeconds(30),
                        }
                    )
                    .AsTask();
                await normalization.Entered.Task.WaitAsync(timeout);
            }
            var preflight = (hasFinishingTarget ? target : source).Runtime;
            using var deadline = new CancellationTokenSource(timeout);
            var result = await store.CountReadsAsync(() =>
                preflight.PreflightRetireAsync(
                    ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    preflight.Registration.ApplicationVersion,
                    deadline.Token
                )
            );
            Assert.Equal(
                hasFinishingTarget ? ZLinkFrameworkRelocationReason.OperationInProgress : null,
                result.Reason
            );
            Assert.True(
                result.Reads == expectedReads,
                $"SPOT authority reads: expected {expectedReads}, actual {result.Reads}."
            );
        }
        finally
        {
            normalization.Release.TrySetResult();
            if (relocation is not null)
                Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, (await relocation).Outcome);
        }
    }
}
