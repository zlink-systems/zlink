using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CapturedActorRequestReturnsTerminalToOriginalRequesterAcrossDeferredCrossNodeJoin(
        bool independentRequester
    )
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        using var backlog = new SpotWideMemberSealCaptureTests.HandoffBacklogLoggerProvider();
        var timeout = TimeSpan.FromSeconds(15);
        var releaseCapture = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var trace = new RelocationBehaviorTrace
        {
            SourceActorCaptureGate = releaseCapture.Task,
            CaptureNodes = ["source", "target"],
            RestoreNodes = ["target", "destination"],
        };
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        trace.ReleaseSourceLeave.SetResult();

        const string mesh = RelocationBehaviorHost.MeshName;
        const string room = "reply-room";
        const string joinOnlyRoom = "reply-join-only";
        const string joinOnlyType = "reply-join-only-type";
        const string marker = "saved";
        var actorId = $"captured-reply-{Guid.NewGuid():N}";
        var actorKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var locations = new ZLinkInMemoryProviderLocationStore();
        var terminalGate = new CapturedReplyTerminalGate(locations, actorKey.Value);
        var relocations = new SynchronizedRelocationStore();
        var repository = new ZLinkProviderLocationRepository(locations);

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locations,
            relocations,
            registerTargetSpot: true,
            relocateSpots: true,
            configureServices: services =>
                services.AddLogging(logging => logging.AddProvider(backlog))
        );
        var initialRoom = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(room, RelocationBehaviorHost.SpotType)
            .InMesh(mesh)
            .Request(ZLinkMessage.Empty)
            .Timeout(timeout)
            .Async();
        Assert.Equal(source.LocalNodeRid, initialRoom.Spot.NodeRid);
        Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(mesh)
                .Request(new BehaviorCreate(7))
                .Timeout(timeout)
                .Async()
        );
        var sourceClient = source.Services.GetRequiredService<IZLinkActorClient>();
        var scheduled = await sourceClient
            .RequestToActor(actorId, new BeginBehaviorJoin(room))
            .Timeout(timeout)
            .Async<BehaviorAck>();
        Assert.Equal("join", scheduled.Marker);
        await trace.WaitAsync("publicJoinCompleted");
        var initialJoin = Assert.IsType<ZLinkActorJoinCompletion.Accepted>(
            Assert.Single(trace.JoinCompletions).Completion
        );
        Assert.Equal(source.LocalNodeRid, initialJoin.Actor.NodeRid);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            terminalGate,
            relocations,
            registerTargetSpot: true,
            relocateSpots: true
        );
        await using var destination = await RelocationBehaviorHost.StartAsync(
            "destination",
            trace,
            locations,
            relocations,
            registerTargetSpot: true,
            spotType: joinOnlyType
        );
        var joinRoom = await destination
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(joinOnlyRoom, joinOnlyType)
            .InMesh(mesh)
            .Request(ZLinkMessage.Empty)
            .Timeout(timeout)
            .Async();
        Assert.Equal(destination.LocalNodeRid, joinRoom.Spot.NodeRid);
        await WaitUntilAsync(
            () =>
                ReadyPeer(source, target)
                && ReadyPeer(target, source)
                && ReadyPeer(target, destination)
                && ReadyPeer(destination, target)
                && (
                    !independentRequester
                    || (ReadyPeer(source, destination) && ReadyPeer(destination, source))
                ),
            timeout
        );

        trace.OnTargetWork = (actor, request) =>
        {
            Assert.Equal(marker, request.Marker);
            actor
                .Context.JoinSpot(joinOnlyRoom, new BeginBehaviorJoin(joinOnlyRoom))
                .Timeout(timeout)
                .Defer();
        };
        var requester = independentRequester ? destination : source;
        var moving = source
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = timeout,
                }
            )
            .AsTask();
        try
        {
            var captureStarted = trace.WaitAsync("sourceStateCapturing");
            Assert.Same(captureStarted, await Task.WhenAny(captureStarted, moving));
            await captureStarted;
            var request = requester
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BehaviorWork(marker))
                .Timeout(timeout)
                .Async<BehaviorAck>();
            var reply = request.AsTask();
            await request.Admitted.AsTask().WaitAsync(timeout);
            await backlog.WaitForBacklogAsync(actorId).WaitAsync(timeout);
            Assert.False(reply.IsCompleted);
            releaseCapture.SetResult();

            var moved = await moving.WaitAsync(timeout);
            Assert.True(
                moved.Outcome == ZLinkFrameworkRelocationOutcome.Relocated,
                moved.ToString()
            );
            Assert.Equal(marker, (await reply.WaitAsync(timeout)).Marker);
            Assert.Equal([marker], trace.DeliveredMarkers);

            var terminal = await terminalGate.Writing.Task.WaitAsync(timeout);
            Assert.Equal(requester.LocalNodeRid, RoutingId.FromHex(terminal.Entry.Source.NodeRid));
            var pending = Assert.Single(
                await repository.ReadRelocationRepliesAsync(terminal.RelocationId, default)
            );
            Assert.Equal(terminal.Key, pending.Key);
            Assert.Equal(ZLinkRelocationCompletionDelivery.Pending, pending.Entry.Delivery);

            await WaitUntilAsync(() => trace.JoinCompletions.Count >= 2, timeout);
            var joins = trace.JoinCompletions.ToArray();
            Assert.Equal(2, joins.Length);
            var joined = Assert.IsType<ZLinkActorJoinCompletion.Accepted>(joins[1].Completion);
            Assert.Equal("destination", joins[1].Node);
            Assert.Equal(destination.LocalNodeRid, joined.Actor.NodeRid);
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(actorKey)
            );
            Assert.Equal(destination.LocalNodeRid, authority.Snapshot.Allocation.Descriptor.Rid);

            terminalGate.Release.SetResult();
            await terminalGate.Stored.Task.WaitAsync(timeout);
            Assert.Equal(
                terminal,
                Assert.Single(
                    await repository.ReadRelocationRepliesAsync(terminal.RelocationId, default)
                )
            );
        }
        finally
        {
            releaseCapture.TrySetResult();
            terminalGate.Release.TrySetResult();
        }

        static bool ReadyPeer(RelocationBehaviorHost host, RelocationBehaviorHost peer)
        {
            var status = host.Services.GetRequiredService<IZLinkRouteMeshRuntime>().GetStatus(mesh);
            return status.IsReady
                && status.Peers.Any(candidate =>
                    candidate.NodeRid == peer.LocalNodeRid
                    && candidate.State == ZLinkPeerState.Ready
                );
        }
    }

    private sealed class CapturedReplyTerminalGate(IZLinkLocationStore inner, string actorKey)
        : IZLinkLocationStore
    {
        internal TaskCompletionSource<ZLinkRelocationReplyRow> Writing { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Stored { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            var terminal = request
                .Mutations.OfType<ZLinkStoreMutation.Put>()
                .Where(put =>
                    put.Key.Value.StartsWith("$zlink/relocation-reply/", StringComparison.Ordinal)
                )
                .Select(put => ZLinkRelocationReplyRow.Decode(put.Key, put.Bytes))
                .SingleOrDefault(row =>
                    row.Entry.ObjectKey == actorKey
                    && row.Entry.Delivery == ZLinkRelocationCompletionDelivery.TerminalReceived
                );
            if (terminal is not null)
            {
                Writing.TrySetResult(terminal);
                await Release.Task.WaitAsync(cancellationToken);
            }

            var result = await inner.WriteAsync(request, cancellationToken);
            if (terminal is not null && result is ZLinkStoreWriteResult.Applied)
                Stored.TrySetResult();
            return result;
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
