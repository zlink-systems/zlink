using System.Collections.Concurrent;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(true, 1)]
    [InlineData(false, 1)]
    public async Task SpotSourceLookupAwaitPreservesOrderAndCountsStoreScans(
        bool secondRequestHasHint,
        int expectedScans
    )
    {
        HeldSourceDescriptorStore? heldStore = null;
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(SourceDiscoverySpot),
            locationStoreWrapper: inner => heldStore = new HeldSourceDescriptorStore(inner)
        );
        var pending = new List<Task>();
        try
        {
            var store = runtime.Registration.Locations.ResolveStore()!;
            var local = Assert.Single(await store.ListAllMeshNodesAsync("entry"));
            var source = local with
            {
                Rid = RoutingId.From("new-client"),
                LifecycleGeneration = 77,
                Endpoint = "inproc://new-client",
                DescriptorRevision = 1,
            };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await store.UpdateMeshNodeAsync(source, ZLinkLocationWriteIntent.NewClaim)).Status
            );
            await runtime.CreateAsync<SourceDiscoverySpot>();
            var spot = Assert.IsType<SourceDiscoverySpot>(
                Assert.Single(runtime.GetSpotNodeRuntime("entry").Catalog.Spots).UserSpot
            );
            var native = Assert.Single(node.CreatedSpots);
            var sourceFence = new ZLinkServiceWireCodec.RequestSourceFence(
                source.OwnerId,
                checked((ulong)source.LeaseGeneration),
                source.Rid,
                source.LifecycleGeneration
            );
            var info = new ZLinkBackendSpotDispatchInfo(
                Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
                ZLinkBackendSpotDispatchEvent.RouteReadable
            );
            void Receive(int sequence, ZLinkServiceWireCodec.RequestSourceFence? hint)
            {
                var header = ZLinkClientCallCodec.CreateEnvelope(
                    ZLinkMessageKind.Request,
                    "entry",
                    nameof(SourceDiscoveryPing)
                );
                native.Routes.Enqueue(
                    new ZLinkBackendRouteReceived(
                        ZLinkClientCallCodec.EncodeEnvelopeParts(
                            header,
                            new SourceDiscoveryPing(sequence),
                            null
                        ),
                        source.Rid,
                        null,
                        checked((ulong)sequence),
                        _ => SubmitResult.Ok,
                        operationId: new MeshOperationId(77, checked((ulong)sequence)),
                        targetNodeGeneration: local.LifecycleGeneration,
                        authorityOwnerGeneration: 1,
                        ownerLeaseGeneration: checked((ulong)local.LeaseGeneration),
                        sourceNodeGeneration: 77,
                        requestSource: hint
                    )
                );
                pending.Add(native.RaiseDispatch(info).AsTask());
            }

            heldStore!.Arm();
            Receive(1, null);
            await heldStore.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Receive(2, secondRequestHasHint ? sourceFence : null);
            // The hinted B needs no Store work. Give its concurrently
            // submitted ingress turn an opportunity to run while A is held.
            await Task.WhenAny(pending[1], Task.Delay(TimeSpan.FromMilliseconds(100)));
            Assert.Empty(spot.Sequences);
            heldStore.Release.TrySetResult();
            await Task.WhenAll(pending).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(SpinWait.SpinUntil(() => spot.Handled == 2, TimeSpan.FromSeconds(5)));
            Assert.Equal(new[] { 1, 2 }, spot.Sequences.ToArray());
            Assert.Equal(expectedScans, heldStore.ScanCalls);
        }
        finally
        {
            heldStore?.Release.TrySetResult();
            try
            {
                await Task.WhenAll(pending).WaitAsync(TimeSpan.FromSeconds(5));
            }
            finally
            {
                await runtime.StopAsync(CancellationToken.None);
            }
        }
    }

    private sealed class HeldSourceDescriptorStore(IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private int _armed;
        private int _tracking;
        private int _scanCalls;
        internal int ScanCalls => Volatile.Read(ref _scanCalls);
        internal TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal void Arm()
        {
            Volatile.Write(ref _tracking, 1);
            Volatile.Write(ref _armed, 1);
        }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => inner.WriteAsync(request, cancellationToken);

        public async ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (Volatile.Read(ref _tracking) != 0)
                Interlocked.Increment(ref _scanCalls);
            var page = await inner.ScanAsync(request, cancellationToken);
            if (Interlocked.Exchange(ref _armed, 0) != 0)
            {
                Entered.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
            }
            return page;
        }
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task SpotRequestBeforeSourceDiscoveryUsesExactDescriptor(bool matchingLifecycle)
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(SourceDiscoverySpot)
        );
        try
        {
            var store = runtime.Registration.Locations.ResolveStore()!;
            var local = Assert.Single(await store.ListAllMeshNodesAsync("entry"));
            var source = local with
            {
                Rid = RoutingId.From("new-client"),
                LifecycleGeneration = matchingLifecycle ? 77UL : 78UL,
                Endpoint = "inproc://new-client",
                DescriptorRevision = 1,
            };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await store.UpdateMeshNodeAsync(source, ZLinkLocationWriteIntent.NewClaim)).Status
            );
            await runtime.CreateAsync<SourceDiscoverySpot>();
            var spot = Assert.IsType<SourceDiscoverySpot>(
                Assert.Single(runtime.GetSpotNodeRuntime("entry").Catalog.Spots).UserSpot
            );
            var native = Assert.Single(node.CreatedSpots);
            var reply = new TaskCompletionSource<ZLinkMessageKind>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var header = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request,
                "entry",
                nameof(SourceDiscoveryPing)
            );
            native.Routes.Enqueue(
                new ZLinkBackendRouteReceived(
                    ZLinkClientCallCodec.EncodeEnvelopeParts(header, new SourceDiscoveryPing(), null),
                    source.Rid,
                    null,
                    1,
                    parts =>
                    {
                        reply.TrySetResult(ZLinkEnvelopeCodec.DecodeHeader(parts).Kind);
                        return SubmitResult.Ok;
                    },
                    operationId: new MeshOperationId(77, 1),
                    targetNodeGeneration: local.LifecycleGeneration,
                    authorityOwnerGeneration: 1,
                    ownerLeaseGeneration: checked((ulong)local.LeaseGeneration),
                    sourceNodeGeneration: 77,
                    requestSource: null
                )
            );

            await native.RaiseDispatch(
                new ZLinkBackendSpotDispatchInfo(
                    Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
                    ZLinkBackendSpotDispatchEvent.RouteReadable
                )
            );
            Assert.Equal(
                matchingLifecycle ? ZLinkMessageKind.Response : ZLinkMessageKind.Error,
                await reply.Task.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(matchingLifecycle ? 1 : 0, spot.Handled);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed record SourceDiscoveryPing(int Sequence = 0);

    private sealed class SourceDiscoverySpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
        internal int Handled;
        internal ConcurrentQueue<int> Sequences { get; } = new();

        public void Configure() => Context.Handlers.AddPacket<SourceDiscoveryHandler>();
    }

    private sealed class SourceDiscoveryHandler
        : IZLinkSpotRequestHandler<SourceDiscoverySpot, SourceDiscoveryPing, SourceDiscoveryPing>
    {
        public ValueTask<SourceDiscoveryPing> HandleAsync(
            SourceDiscoverySpot spot,
            SourceDiscoveryPing request,
            CancellationToken cancellationToken
        )
        {
            spot.Sequences.Enqueue(request.Sequence);
            Interlocked.Increment(ref spot.Handled);
            return ValueTask.FromResult(request);
        }
    }
}
