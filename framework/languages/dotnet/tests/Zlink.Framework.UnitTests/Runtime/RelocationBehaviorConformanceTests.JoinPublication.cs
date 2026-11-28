using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Actor_join_normalizes_publication_at_the_captured_request_boundary(
        bool captureRequest
    )
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var actorId = $"join-publication-{Guid.NewGuid():N}";
        var trace = new RelocationBehaviorTrace { ActorId = actorId };
        trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace)
        {
            HoldFirstTargetCleaning = captureRequest,
        };
        var roots = new SynchronizedRelocationStore();
        var authority = new ZLinkProviderLocationRepository(store);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var timeout = TimeSpan.FromSeconds(10);
        var handlerEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseHandler = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.BeforeTargetWorkReply = async cancellationToken =>
        {
            handlerEntered.TrySetResult();
            await releaseHandler.Task.WaitAsync(cancellationToken);
        };

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            registerTargetSpot: false
        );
        await CreatePreflightActorAsync(source, actorId);
        Assert.True(source.Runtime.TryGetActorState(actorId, out var sourceState));
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            registerTargetSpot: true
        );
        trace.TargetNodeRid = target.LocalNodeRid;
        var spot = await target
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(
                $"join-publication-spot-{Guid.NewGuid():N}",
                RelocationBehaviorHost.SpotType
            )
            .InMesh(RelocationBehaviorHost.MeshName)
            .Timeout(timeout)
            .Async();
        var joinCallbackState = new TaskCompletionSource<ZLinkAuthorityRelocation?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.OnPublicJoinCompleted = async _ =>
            joinCallbackState.TrySetResult(await ReadRelocationAsync());

        var client = source.Services.GetRequiredService<IZLinkActorClient>();
        var join = client
            .RequestToActor(actorId, new BeginBehaviorJoin(spot.Spot.SpotId))
            .Timeout(timeout)
            .Async<BehaviorAck>()
            .AsTask();
        Task<BehaviorAck>? request = null;
        try
        {
            await trace.WaitAsync("relocationRequested");
            if (captureRequest)
            {
                request = client
                    .RequestToActor(actorId, new BehaviorWork("saved"))
                    .Timeout(timeout)
                    .Async<BehaviorAck>()
                    .AsTask();
                await WaitUntilAsync(() =>
                    sourceState.Handoff._lane.RunAsync(() => sourceState.Handoff._frames.Count != 0)
                );
            }
            trace.ReleaseJoinHandler.TrySetResult();
            await join.WaitAsync(timeout);

            var atCallback = await joinCallbackState.Task.WaitAsync(timeout);
            if (request is not null)
            {
                Assert.Equal(
                    ServiceWireCodec.RelocationPhase.Activated,
                    Assert.IsType<ZLinkAuthorityRelocation>(atCallback).Phase
                );
                await handlerEntered.Task.WaitAsync(timeout);
                Assert.False(request.IsCompleted);
                Assert.True(target.Runtime.TryGetActorState(actorId, out var actor));
                Assert.True(actor.Handoff.HasPendingCapturedRequests());
                Assert.False(store.TargetCleaningStarted.Task.IsCompleted);
                Assert.Equal(atCallback, await ReadRelocationAsync());
                releaseHandler.TrySetResult();
                Assert.Equal("saved", (await request.WaitAsync(timeout)).Marker);
                await store.TargetCleaningStarted.Task.WaitAsync(timeout);
                store.ReleaseTargetCleaning.TrySetResult();
            }
            else
            {
                Assert.Null(atCallback);
                Assert.False(handlerEntered.Task.IsCompleted);
            }

            await WaitForSteadyActorAuthorityAsync(store, actorId).WaitAsync(timeout);
        }
        finally
        {
            trace.ReleaseJoinHandler.TrySetResult();
            releaseHandler.TrySetResult();
            store.ReleaseTargetCleaning.TrySetResult();
        }

        async ValueTask<ZLinkAuthorityRelocation?> ReadRelocationAsync() =>
            Assert
                .IsType<ZLinkAuthorityReadResult.Found>(await authority.ReadAuthorityAsync(key))
                .Snapshot.Relocation;
    }
}
