using System.Reflection;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class MeshNodeLifecycleStateTests
{
    private const string Mesh = "pmdotnet-close-fence";

    [Fact]
    public async Task DisposedMeshStaysStoppedAfterLateHello()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, Mesh);
        node.AddChannel(Mesh);
        const string endpoint = "tcp://127.0.0.1:20005";
        var hello = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            Mesh,
            endpoint,
            lifecycleGeneration: 7,
            descriptorRevision: 1,
            new Dictionary<string, uint> { [Mesh] = 1 },
            objectRole: (byte)ZLinkMeshNodeObjectRole.Server
        );
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(hello, out _, out var admission, out _)
        );
        await node.DisposeAsync();
        Assert.Equal(MeshNodeState.Stopped, node.Status().State);
        // Deliver the already-received control record after the close turn.
        // No socket, receive loop, clock advancement or waiting is involved.
        typeof(ZLinkManagedMeshNode)
            .GetMethod("ProcessAdmission", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(
                node,
                [
                    RoutingId.From("late-hello-peer"),
                    1UL,
                    ServiceWireConstants.Command.Hello,
                    admission,
                    null,
                ]
            );
        Assert.Equal(MeshNodeState.Stopped, node.Status().State);
    }

    [Fact]
    public async Task DisposedMeshStaysStoppedAfterLateDraining()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, Mesh);
        await node.DisposeAsync();
        Assert.Equal(MeshNodeState.Stopped, node.Status().State);
        node.PublishDraining();
        Assert.Equal(MeshNodeState.Stopped, node.Status().State);
    }
}
