using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public async Task SpotWide_member_replay_failure_reaches_the_target_cutover()
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var timeout = TimeSpan.FromSeconds(10);
        var actorId = $"replay-failure-{Guid.NewGuid():N}";
        var spotId = $"replay-failure-spot-{Guid.NewGuid():N}";
        var releaseCapture = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var trace = new RelocationBehaviorTrace { SourceActorCaptureGate = releaseCapture.Task };
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        var probe = new CanonicalRelocationTransportProbe(trace, holdTargetReady: true);
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            canonicalTransportProbe: probe,
            relocateSpots: true
        );
        await CreateAndJoinSpotWideMemberAsync(source, spotId, actorId, timeout);
        await trace.WaitAsync("publicJoinCompleted");
        Assert.True(source.Runtime.TryGetActorState(actorId, out var sourceActor));
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            true,
            canonicalTransportProbe: probe,
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
        trace.InitializeTarget = async (_, _) =>
        {
            Assert.True(target.Runtime.TryGetActorState(actorId, out var actor));
            var frame = Assert.Single(
                await actor.Handoff._lane.RunAsync(() => actor.Handoff._frames.ToArray())
            );
            // Simulate loss of the payload owner after the FIFO reserved its replay.
            actor.Handoff.ClaimDurableFrame(frame);
        };
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
        try
        {
            await trace.WaitAsync("sourceStateCapturing");
            var client = target.Services.GetRequiredService<IZLinkActorClient>();
            await client
                .SendToActor(
                    actorId,
                    new NextBehaviorJoinSend(new NextBehaviorJoin(spotId, timeout))
                )
                .Async()
                .AsTask()
                .WaitAsync(timeout);
            await WaitUntilAsync(() =>
                sourceActor.Handoff._lane.RunAsync(() => sourceActor.Handoff._frames.Count == 1)
            );
            releaseCapture.TrySetResult();
            probe.ReleasePrepareCall.TrySetResult();
            probe.ReleaseTargetReadySend.TrySetResult();
            probe.ReleaseCutoverSend.TrySetResult();

            var failure = Assert.IsType<InvalidOperationException>(
                await probe.TargetCutoverFailure.Task.WaitAsync(timeout)
            );
            Assert.Contains("changed its payload owner before claim", failure.Message);
            Assert.Equal(
                ZLinkFrameworkRelocationOutcome.Relocated,
                (await relocation.WaitAsync(timeout)).Outcome
            );
            await WaitForSteadyActorAuthorityAsync(store, actorId).WaitAsync(timeout);
        }
        finally
        {
            releaseCapture.TrySetResult();
            probe.ReleasePrepareCall.TrySetResult();
            probe.ReleaseTargetReadySend.TrySetResult();
            probe.ReleaseCutoverSend.TrySetResult();
            await ((Task)relocation).ConfigureAwait(
                ConfigureAwaitOptions.SuppressThrowing
                    | ConfigureAwaitOptions.ContinueOnCapturedContext
            );
        }
    }
}
