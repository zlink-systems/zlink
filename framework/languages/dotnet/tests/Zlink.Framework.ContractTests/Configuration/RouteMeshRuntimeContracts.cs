using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Configuration;

public sealed class RouteMeshRuntimeContracts
{
    [Fact]
    [ContractExample(typeof(IZLinkRouteMeshRuntime))]
    public async Task Runtime_exposes_complete_immutable_status_values()
    {
        IZLinkRouteMeshRuntime runtime = new ExampleMeshRuntime();

        var status = runtime.GetStatus("orders");
        Assert.Equal("orders", status.MeshName);
        Assert.Equal(ZLinkTopologyState.Ready, status.State);
        Assert.True(status.IsReady);
        Assert.Equal(ZLinkPeerState.Ready, Assert.Single(status.Peers).State);
        Assert.True(Assert.Single(status.Channels).IsReady);
        Assert.Equal(4, status.Placement.ActiveActorCount);
        Assert.Equal(3, status.Placement.ActiveSpotCount);

        await foreach (var observed in runtime.ObserveAsync("orders"))
        {
            Assert.Equal("orders", observed.Status.MeshName);
            Assert.True(observed.Status.Sequence > status.Sequence);
            break;
        }

        Assert.Equal(
            new[] { "GetStatus", "ObserveAsync" },
            typeof(IZLinkRouteMeshRuntime)
                .GetMethods()
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());
    }

    [Fact]
    public void Public_status_omits_internal_transport_and_store_fields()
    {
        var publicProperties = new[]
        {
            typeof(ZLinkRouteMeshStatus),
            typeof(ZLinkPeerStatus),
            typeof(ZLinkPlacementStatus),
            typeof(ZLinkChannelStatus)
        }.SelectMany(static type => type.GetProperties())
            .Select(static property => property.Name)
            .ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("Endpoint", publicProperties);
        Assert.DoesNotContain("LifecycleGeneration", publicProperties);
        Assert.DoesNotContain("DescriptorRevision", publicProperties);
        Assert.DoesNotContain("Capacity", publicProperties);
        Assert.DoesNotContain("OwnerLease", publicProperties);
    }

    private sealed class ExampleMeshRuntime : IZLinkRouteMeshRuntime
    {
        private static readonly RoutingId PeerRid = RoutingId.From("orders-b");

        public ZLinkRouteMeshStatus GetStatus(string meshName) =>
            Create(meshName, sequence: 1);

        public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
            string meshName,
            [System.Runtime.CompilerServices.EnumeratorCancellation]
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Yield();
            yield return new ZLinkObservedStatus<ZLinkRouteMeshStatus>(
                Create(meshName, sequence: 2),
                default);
        }

        private static ZLinkRouteMeshStatus Create(
            string meshName,
            ulong sequence) =>
            new(
                meshName,
                ZLinkTopologyState.Ready,
                IsReady: true,
                ReadyPeerCount: 1,
                [new ZLinkChannelStatus("orders", true, 2)],
                [new ZLinkPeerStatus(PeerRid, ZLinkPeerState.Ready, null)],
                new ZLinkPlacementStatus(true, 4, 3, null),
                sequence,
                DateTimeOffset.UtcNow);
    }
}
