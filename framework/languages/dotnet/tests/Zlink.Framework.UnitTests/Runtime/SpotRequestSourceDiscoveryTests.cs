using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    // Location runtime §9.3: the dispatch pump records the source owner values when this owner
    // accepts the route. The Spot path uses the recorded values and does not read the Store again;
    // a route without them ends Unavailable (Session·Actor binding §4).
    public async Task SpotRouteUsesTheRecordedSourceOwnerValues(bool recorded)
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
                LifecycleGeneration = 77UL,
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
                    ZLinkClientCallCodec.EncodeEnvelopeParts(
                        header,
                        new SourceDiscoveryPing(),
                        null
                    ),
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
                    requestSource: recorded
                        ? new ZLinkServiceWireCodec.RequestSourceFence(
                            source.OwnerId,
                            checked((ulong)source.LeaseGeneration),
                            source.Rid,
                            source.LifecycleGeneration
                        )
                        : null
                )
            );

            await native.RaiseDispatch(
                new ZLinkBackendSpotDispatchInfo(
                    Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobOrigin.Remote,
                    ZLinkBackendSpotDispatchEvent.RouteReadable
                )
            );
            Assert.Equal(
                recorded ? ZLinkMessageKind.Response : ZLinkMessageKind.Error,
                await reply.Task.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(recorded ? 1 : 0, spot.Handled);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed record SourceDiscoveryPing;

    private sealed class SourceDiscoverySpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
        internal int Handled;

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
            Interlocked.Increment(ref spot.Handled);
            return ValueTask.FromResult(request);
        }
    }
}
