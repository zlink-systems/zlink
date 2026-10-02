using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ServiceWireInstanceReadyCodecTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Ready_route_preserves_only_the_call_intent(bool instanceIntent)
    {
        var source = RoutingId.From(new byte[] { 1, 2, 3 });
        var snapshot = new ZLinkSpotHandleSnapshot(
            "mesh",
            RoutingId.From(new byte[] { 4, 5, 6 }),
            "ready-spot",
            7,
            ZLinkSpotKind.Instance,
            8,
            9,
            10,
            "owner",
            "version"
        );
        var route = Assert.IsType<ServiceWireCodec.InstanceRouteV1Case0>(
            ZLinkServiceWireCodec.CreateInstanceReadyRoute(snapshot, instanceIntent)
        );
        var head = ZLinkServiceWireCodec.EncodeInstanceSpotReady(
            route,
            11,
            source,
            string.Empty,
            new MeshOperationId(12, 13),
            request: true,
            hasMetadata: false
        );

        Assert.True(
            ZLinkServiceWireCodec.TryDecodeInstanceSpotReady(
                head,
                out var decoded,
                out var decodedSource
            )
        );
        Assert.Equal(instanceIntent, decoded.InstanceIntent);
        Assert.Equal(source, decodedSource);
        Assert.Equal(snapshot.Generation, decoded.TargetSpotGeneration);
        Assert.Equal(snapshot.AuthorityOwnerGeneration, decoded.AuthorityOwnerGeneration);
        Assert.Equal(snapshot.OwnerLeaseGeneration, decoded.OwnerLeaseGeneration);

        var routeLength = (head[6] << 8) | head[7];
        head[8 + routeLength - 1] = 2;
        Assert.False(ZLinkServiceWireCodec.TryDecodeInstanceSpotReady(head, out _, out _));
    }

    [Fact]
    public void Ready_send_has_the_schema_required_zero_operation()
    {
        var source = RoutingId.From(new byte[] { 1, 2, 3 });
        var snapshot = new ZLinkSpotHandleSnapshot(
            "mesh",
            source,
            "ready-send",
            7,
            ZLinkSpotKind.Instance,
            8,
            9,
            10,
            "owner",
            "version"
        );
        var route = Assert.IsType<ServiceWireCodec.InstanceRouteV1Case0>(
            ZLinkServiceWireCodec.CreateInstanceReadyRoute(snapshot, true)
        );
        var head = ZLinkServiceWireCodec.EncodeInstanceSpotReady(
            route,
            11,
            source,
            string.Empty,
            default,
            request: false,
            hasMetadata: false
        );

        Assert.True(ZLinkServiceWireCodec.TryDecodeInstanceSpotReady(head, out var decoded, out _));
        Assert.True(decoded.InstanceIntent);
        Assert.Equal(default, decoded.OperationId);
        Assert.Equal((ulong)0, decoded.Correlation);
    }
}
