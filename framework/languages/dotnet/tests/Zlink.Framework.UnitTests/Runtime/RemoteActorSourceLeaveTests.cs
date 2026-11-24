using Microsoft.Extensions.DependencyInjection;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RemoteActorSourceLeaveTests(ITestOutputHelper output)
{
    [Fact]
    public async Task Disconnect_before_source_leave_acceptance_reports_failure_after_join_completes()
    {
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            registerTargetSpot: false
        );
        var actorId = $"source-leave-{Guid.NewGuid():N}";
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        Assert.Equal(source.LocalNodeRid, created.Actor.NodeRid);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            registerTargetSpot: true
        );
        var spot = await target
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate($"source-leave-spot-{Guid.NewGuid():N}", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var backend = Assert.IsType<ZLinkBackendSpotNodeWrapper>(
            target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName).Node
        );
        var node = backend.NativeNode;
        var scheduler = node._routedSubmitScheduler.ExclusiveScheduler;
        var workerPaused = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var releaseWorker = new ManualResetEventSlim();
        var failures = new AuditRuntimeFailureReporter();
        target.Runtime.ErrorSink.UnhandledCallbackException += failures.ReportHandlerException;
        RoutingId physicalSource = default;
        Task? pause = null;
        trace.OnTargetJoined = async _ =>
        {
            physicalSource = node.RequireDirectPeer(source.LocalNodeRid).PhysicalRoutingId;
            pause = Task.Factory.StartNew(
                () =>
                {
                    workerPaused.SetResult();
                    releaseWorker.Wait();
                },
                CancellationToken.None,
                TaskCreationOptions.None,
                scheduler
            );
            await workerPaused.Task.WaitAsync(TimeSpan.FromSeconds(5));
        };

        try
        {
            _ = await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spot.Spot.SpotId))
                .Async<BehaviorAck>();
            await trace.WaitAsync("publicJoinCompleted");
            var completion = Assert.Single(trace.JoinCompletions);
            Assert.Equal("target", completion.Node);
            Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion.Completion);
            Assert.Empty(failures.Failures);
            var socket = node._socket!;
            Assert.Contains(socket.RoutesSnapshot(), route => route.RoutingId == physicalSource);
            socket.DisconnectRid(physicalSource);
            using var disconnected = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            while (socket.RoutesSnapshot().Any(route => route.RoutingId == physicalSource))
                await Task.Delay(1, disconnected.Token);
            output.WriteLine(
                "Join accepted while source-leave submission was paused. Source route disconnected."
            );
        }
        finally
        {
            releaseWorker.Set();
            if (pause is not null)
                await pause;
        }

        await Task
            .Factory.StartNew(
                static () => { },
                CancellationToken.None,
                TaskCreationOptions.None,
                scheduler
            )
            .WaitAsync(TimeSpan.FromSeconds(5));
        var failure = Assert.IsType<ZlinkSubmitException>(Assert.Single(failures.Failures));
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, failure.Result);
        output.WriteLine($"Source-leave acceptance failed once: {failure.Result}.");
    }
}
