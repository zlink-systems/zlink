using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RemoteActorJoinStopDeadlineTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Remote_join_reports_unavailable_when_authority_is_unsettled_at_stop_deadline(
        bool forceStopRuntime
    )
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var timeout = TimeSpan.FromSeconds(5);
        var actorId = $"stop-deadline-actor-{Guid.NewGuid():N}";
        var spotId = $"stop-deadline-spot-{Guid.NewGuid():N}";
        var trace = new RelocationBehaviorTrace { ActorId = actorId, ExpectedJoinAccepted = false };
        var transport = new CanonicalRelocationTransportProbe(trace, holdTargetReady: true);
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace)
        {
            SourceAbortWriteFailure = new IOException("The Preserve outcome is unknown."),
        };
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var sourceStore = new ActorAuthorityCallCounter(store, key);
        var roots = new SynchronizedRelocationStore();

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            sourceStore,
            roots,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var actor = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        trace.SourceNodeRid = actor.Actor.NodeRid;
        trace.SourceObjectGeneration = actor.Actor.ObjectGeneration;
        await store.ObserveActorAuthorityAsync(actorId);
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        trace.TargetNodeRid = target.LocalNodeRid;
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var sourceState = await source.Runtime.EnsureStartedStateAsync(CancellationToken.None);
        Task? forceStop = null;
        try
        {
            trace.ReleaseJoinHandler.SetResult();
            await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spotId, TimeSpan.FromSeconds(2)))
                .Async<BehaviorAck>();
            await transport.PrepareCallStarted.Task.WaitAsync(timeout);
            store.HeldSourceAbort = transport.RelocationId;
            transport.ReleaseTargetReadySend.SetResult();
            transport.ReleasePrepareCall.SetResult();

            // Holding cutover keeps Target from committing while the Restore deadline
            // brings Source to Preserve. The in-flight write fails at the stop deadline.
            await transport.CutoverSendStarted.Task.WaitAsync(timeout);
            await store.SourceAbortWriteStarted.Task.WaitAsync(timeout);
            Assert.Empty(trace.JoinCompletions);
            var callsBeforeStop = sourceStore.Calls;
            using var releasePreserve = sourceState.ForceStopTokenSource.Token.Register(() =>
                store.ReleaseSourceAbortWrite.TrySetResult()
            );
            if (forceStopRuntime)
                forceStop = source.Runtime.ForceStopAsync(CancellationToken.None).AsTask();
            else
                sourceState.ForceStopTokenSource.Cancel();
            await trace.WaitAsync("publicJoinCompleted");
            if (forceStop is not null)
                await forceStop.WaitAsync(timeout);

            var completion = Assert.Single(trace.JoinCompletions);
            Assert.Equal("source", completion.Node);
            Assert.Equal(
                ZLinkFrameworkErrorKind.Unavailable,
                Assert.IsType<ZLinkActorJoinCompletion.Failed>(completion.Completion).Kind
            );
            Assert.Equal(callsBeforeStop, sourceStore.Calls);
            var unsettled = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(key)
            );
            Assert.Equal(store.CapturedSourceAbort!.StoreVersion, unsettled.Snapshot.StoreVersion);
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            store.ReleaseSourceAbortWrite.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            if (forceStop is not null)
                await forceStop.WaitAsync(timeout);
        }
    }

    private sealed class ActorAuthorityCallCounter(IZLinkLocationStore inner, ZLinkAuthorityKey key)
        : IZLinkLocationStore
    {
        private readonly ZLinkStoreKey _authorityKey =
            ZLinkProviderLocationRepository.AuthorityMetaKey(key);
        private int _calls;

        internal int Calls => Volatile.Read(ref _calls);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (key == _authorityKey)
                Interlocked.Increment(ref _calls);
            return inner.ReadAsync(key, cancellationToken);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                request.Mutations.Any(mutation =>
                    mutation is ZLinkStoreMutation.Put put && put.Key == _authorityKey
                    || mutation is ZLinkStoreMutation.Delete delete && delete.Key == _authorityKey
                )
            )
                Interlocked.Increment(ref _calls);
            return inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
