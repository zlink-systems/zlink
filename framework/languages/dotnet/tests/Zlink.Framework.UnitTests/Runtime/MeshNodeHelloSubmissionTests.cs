using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class MeshNodeHelloSubmissionTests
{
    private const string MeshName = "hello-submission";
    private static readonly TimeSpan ObservationTimeout = TimeSpan.FromSeconds(2);

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task UnansweredHello_IsSubmittedOnceOnTheSelectedRoute(bool knownRoutingId)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"hello-peer-{suffix}");
        peer.SetRoutingId(peerRid);
        var endpoint = $"inproc://hello-peer-{suffix}";
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(RoutingId.From($"hello-local-{suffix}"));
        node.SetBind($"inproc://hello-local-{suffix}");
        node.AddChannel(MeshName);
        node.Start();
        var intent = node.ConnectPeer(endpoint, knownRoutingId ? peerRid : null);

        Assert.True(ReceiveHello(peer, poller));
        var route = Assert.Single(peer.RoutesSnapshot());
        var repeated = ReceiveHello(peer, poller);

        Assert.Equal(route.RouteGeneration, Assert.Single(peer.RoutesSnapshot()).RouteGeneration);
        Assert.False(repeated, "The same selected route must not receive a second HELLO.");

        node.RemovePeerConnection(intent);
        node.ConnectPeer(endpoint, knownRoutingId ? peerRid : null);
        Assert.True(ReceiveHello(peer, poller));
        Assert.NotEqual(
            route.RouteGeneration,
            Assert.Single(peer.RoutesSnapshot()).RouteGeneration
        );
        Assert.False(ReceiveHello(peer, poller), "The replacement route must receive one HELLO.");
    }

    private static bool ReceiveHello(IRouterSocket peer, IPoller poller)
    {
        var started = Stopwatch.GetTimestamp();
        var events = new PollEvent[1];
        while (true)
        {
            var remaining = ObservationTimeout - Stopwatch.GetElapsedTime(started);
            if (remaining <= TimeSpan.Zero || poller.Wait(events, remaining) == 0)
                return false;
            using var received = Received.Create();
            if (!peer.Recv(received, RecvFlags.DontWait))
                continue;
            if (
                ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                    received.FirstPart().AsSpan(),
                    out var command,
                    out _,
                    out _
                )
                && command == ServiceWireConstants.Command.Hello
            )
                return true;
        }
    }
}
