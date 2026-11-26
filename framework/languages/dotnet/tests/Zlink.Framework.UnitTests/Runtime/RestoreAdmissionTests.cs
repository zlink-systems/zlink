using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RestoreAdmissionTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Command40_checks_local_owner_deadline_before_store_prepare(bool expired)
    {
        using var listener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var time = new ManualTimeProvider();
        var trace = new RelocationBehaviorTrace();
        var transport = new CanonicalRelocationTransportProbe(trace);
        var blobs = new SynchronizedRelocationStore();
        var actorId = $"restore-admission-actor-{Guid.NewGuid():N}";
        var spotId = $"restore-admission-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;
        var store = new PrepareObservingStore(actorId);

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            blobs,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async()
        );
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            blobs,
            registerTargetSpot: true,
            canonicalTransportProbe: transport,
            locationTimeProvider: time
        );
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        try
        {
            var join = source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spotId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>()
                .AsTask();
            await trace.WaitAsync("relocationRequested");
            trace.ReleaseJoinHandler.TrySetResult();
            await join.WaitAsync(TimeSpan.FromSeconds(15));

            await transport.PrepareCallStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var locations = target.Services.GetRequiredService<ZLinkLocationRuntime>();
            Assert.True(locations.IsOwnerAdmissionOpen);
            if (expired)
            {
                var ttl = target.Services.GetRequiredService<ZLinkLocationOptions>().OwnerLeaseTtl;
                time.AdvanceMonotonicOnly(ttl);
                Assert.False(locations.IsOwnerAdmissionOpen);
            }

            transport.ReleasePrepareCall.TrySetResult();
            var ready = transport.ReadyReplyReceived.Task;
            var rejected = transport.PrepareFailure.Task;
            var response = await Task.WhenAny(ready, rejected).WaitAsync(TimeSpan.FromSeconds(5));
            if (expired)
            {
                Assert.Same(rejected, response);
                Assert.Equal(
                    ZLinkFrameworkErrorKind.Unavailable,
                    Assert.IsType<ZLinkFrameworkException>(await rejected).Kind
                );
                Assert.Equal(0, store.PrepareWrites);
                Assert.DoesNotContain("targetStateRestored", trace.Events);
            }
            else
            {
                Assert.Same(ready, response);
                Assert.Equal(1, store.PrepareWrites);
                Assert.Contains("targetStateRestored", trace.Events);
            }
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseJoinHandler.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    private sealed class PrepareObservingStore(string actorId) : IZLinkLocationStore
    {
        private readonly ZLinkInMemoryProviderLocationStore _store = new();
        private readonly ZLinkStoreKey _authorityKey =
            ZLinkProviderLocationRepository.AuthorityMetaKey(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
            );
        private int _prepareWrites;

        internal int PrepareWrites => Volatile.Read(ref _prepareWrites);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => _store.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => _store.ScanAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            foreach (var put in request.Mutations.OfType<ZLinkStoreMutation.Put>())
            {
                if (put.Key != _authorityKey)
                    continue;
                using var record = JsonDocument.Parse(put.Bytes);
                if (
                    record.RootElement.TryGetProperty("aggregate", out var aggregate)
                    && ZLinkAuthorityAggregateCodec.Read(aggregate)?.Phase
                        == ServiceWireCodec.RelocationPhase.Prepared
                )
                    Interlocked.Increment(ref _prepareWrites);
            }
            return _store.WriteAsync(request, cancellationToken);
        }
    }
}
