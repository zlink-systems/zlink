using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SpotWide_target_queues_committed_member_requests_in_arrival_order(bool remote)
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();

        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        var actorId = $"relocating-member-{Guid.NewGuid():N}";
        var spotId = $"relocating-spot-{Guid.NewGuid():N}";
        var timeout = TimeSpan.FromSeconds(10);
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        await CreateAndJoinSpotWideMemberAsync(source, spotId, actorId, timeout);
        await trace.WaitAsync("publicJoinCompleted");

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            relocateSpots: true
        );
        var normalization = SpotWideNormalizationGate.Install(
            target.Runtime.Registration.Locations,
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
        );
        normalization.Release.TrySetResult();
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

        await using var observer = remote
            ? await RelocationBehaviorHost.StartAsync(
                "observer",
                trace,
                store,
                roots,
                registerTargetSpot: false
            )
            : null;
        if (observer is not null)
            await WaitUntilAsync(() =>
                observer
                    .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                    .Node.Status()
                    .ActivePeerCount == 2
                && target
                    .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                    .Node.Status()
                    .ActivePeerCount == 2
            );

        var relocation = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30),
                }
            )
            .AsTask();

        var client = (observer ?? target).Services.GetRequiredService<IZLinkActorClient>();
        try
        {
            await normalization.BeforeLocalFence.Task.WaitAsync(timeout);
            Assert.True(target.Runtime.TryGetActorState(actorId, out var actor));
            var first = client
                .RequestToActor(actorId, new BehaviorWork("saved"))
                .Timeout(timeout)
                .Async<BehaviorAck>()
                .AsTask();
            await WaitForBacklogAsync(first, 1);

            var second = client
                .RequestToActor(actorId, new BehaviorWork("temporary"))
                .Timeout(timeout)
                .Async<BehaviorAck>()
                .AsTask();
            await WaitForBacklogAsync(second, 2);
            Assert.Empty(trace.DeliveredMarkers);
            Assert.False(first.IsCompleted);
            Assert.False(second.IsCompleted);

            normalization.ReleaseLocalFence.TrySetResult();
            Assert.Equal("saved", (await first).Marker);
            Assert.Equal("temporary", (await second).Marker);
            Assert.Equal(new[] { "saved", "temporary" }, trace.DeliveredMarkers);

            Task WaitForBacklogAsync(Task<BehaviorAck> request, int count) =>
                WaitUntilAsync(
                    async () =>
                    {
                        if (request.IsCompleted)
                            await request;
                        Assert.False(request.IsCompleted);
                        return await actor.Handoff._lane.RunAsync(() => actor.Handoff._frames.Count)
                            == count;
                    },
                    timeout
                );
        }
        finally
        {
            normalization.ReleaseLocalFence.TrySetResult();
            await ((Task)relocation).ConfigureAwait(
                ConfigureAwaitOptions.SuppressThrowing
                    | ConfigureAwaitOptions.ContinueOnCapturedContext
            );
        }

        Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, (await relocation).Outcome);
    }
}
