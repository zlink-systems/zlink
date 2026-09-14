using System.Reflection;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshPeerTargetClassificationTests(ITestOutputHelper output)
{
    [Fact]
    public async Task ReadyLookupDoesNotMaterializeUnrelatedPeerSnapshots()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "classification");
        var target = SeedPeer(node, 1, ZLinkMeshNodeObjectRole.Server,
            MeshPeerState.Admitted, indexed: true);
        for (ulong intent = 2; intent <= 513; intent++)
            SeedPeer(node, intent, ZLinkMeshNodeObjectRole.Server,
                MeshPeerState.Connecting, indexed: false);
        for (var index = 0; index < 128; index++)
            Assert.Equal(ZLinkRouteMeshTargetClassification.ReadyEligible,
                node.ClassifyPeerTarget(target.RoutingId));

        const int iterations = 64;
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            _ = node.Peers().FirstOrDefault(peer => peer.RoutingId == target.RoutingId);
        var snapshots = GC.GetAllocatedBytesForCurrentThread() - before;
        before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            _ = node.ClassifyPeerTarget(target.RoutingId);
        var lookup = GC.GetAllocatedBytesForCurrentThread() - before;

        output.WriteLine($"managed bytes per target lookup={lookup / iterations}; "
            + $"full peer snapshot lookup={snapshots / iterations}; peers=513");

        Assert.True(lookup < snapshots / 16,
            $"Target lookup allocated {lookup} bytes; full snapshots allocated {snapshots} bytes.");
    }

    [Theory]
    [InlineData((int)ZLinkMeshNodeObjectRole.Server, (int)MeshPeerState.Closed,
        (int)ZLinkRouteMeshTargetClassification.Unknown)]
    [InlineData((int)ZLinkMeshNodeObjectRole.Client, (int)MeshPeerState.NotRequired,
        (int)ZLinkRouteMeshTargetClassification.ObjectClientTarget)]
    public async Task MissingAdmissionUsesConfiguredIntentAndExpectedRid(
        int role, int state, int expected)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "classification");
        var peer = SeedPeer(node, 1, (ZLinkMeshNodeObjectRole)role, (MeshPeerState)state, indexed: false);
        var rid = peer.ExpectedRid!.Value;
        peer.RoutingId = default;

        Assert.Equal((ZLinkRouteMeshTargetClassification)expected, node.ClassifyPeerTarget(rid));
        Assert.Equal(ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyPeerTarget(RoutingId.From("missing")));
    }

    [Fact]
    public async Task LookupReadsReplacementAdmissionWithoutKeepingASecondRoleCache()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "classification");
        var peer = SeedPeer(node, 1, ZLinkMeshNodeObjectRole.Server,
            MeshPeerState.Admitted, indexed: true);
        Assert.Equal(ZLinkRouteMeshTargetClassification.ReadyEligible,
            node.ClassifyPeerTarget(peer.RoutingId));

        peer.Admission = peer.Admission!.Value with
        {
            ObjectRole = (byte)ZLinkMeshNodeObjectRole.Client
        };
        peer.State = MeshPeerState.NotRequired;
        peer.Admitted = false;
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid").Remove(peer.RoutingId);

        Assert.Equal(ZLinkRouteMeshTargetClassification.ObjectClientTarget,
            node.ClassifyPeerTarget(peer.RoutingId));
    }

    private static ZLinkMeshPeer SeedPeer(ZLinkManagedMeshNode node, ulong intent,
        ZLinkMeshNodeObjectRole role, MeshPeerState state, bool indexed)
    {
        var rid = RoutingId.From($"peer-{intent}");
        var endpoint = $"inproc://classification-{intent}";
        var peer = new ZLinkMeshPeer(intent, endpoint, rid,
            ZLinkServiceSecurityIdentity.Plaintext,
            ZLinkServiceConnectionDirection.Outbound)
        {
            RoutingId = rid,
            PhysicalRoutingId = rid,
            State = state,
            Admitted = indexed,
            Admission = new("classification", ZLinkServiceSecurityIdentity.Plaintext,
                endpoint, 1, 1, new Dictionary<string, uint>(), 1, 0,
                (byte)role, 1, 0, 0, 0, 0, new Dictionary<byte, byte[]>(), [])
        };
        Field<Dictionary<ulong, ZLinkMeshPeer>>(node, "_peersByIntent").Add(intent, peer);
        if (indexed)
            Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid").Add(rid, peer);
        return peer;
    }

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)typeof(ZLinkManagedMeshNode)
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(node)!;
}
