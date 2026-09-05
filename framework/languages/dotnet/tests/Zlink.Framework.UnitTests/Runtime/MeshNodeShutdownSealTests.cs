using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

// Spec 30 §14 step 1 (D-097): the mesh node follows the host's shutdown
// admission seal. After the seal it starts no new peer admission (Hello),
// already-admitted peers still receive the Draining Update, and peer loss
// (transport disconnect, liveness expiry, failed control send) never undoes
// the published Draining. Service-wire §5: a repeated Hello/Admit that carries
// the current descriptor is idempotent - it completes the admission again
// without re-admitting the peer or resetting its descriptor/liveness epoch.
public sealed class MeshNodeShutdownSealTests
{
    private const string MeshName = "orders";

    [Fact]
    public void DrainGate_SealedForShutdown_OnlyByTheShutdownOwner()
    {
        var gate = new ZLinkDrainAdmissionGate();
        Assert.False(gate.IsSealedForShutdown);

        // A relocation fence seals application admission but is not a shutdown.
        Assert.True(gate.TryBeginRelocationFence(_ =>
        {
            gate.Seal();
            return true;
        }));
        Assert.True(gate.IsSealed);
        Assert.False(gate.IsSealedForShutdown);

        gate.ClaimShutdown();
        Assert.True(gate.IsSealedForShutdown);

        gate.Reset();
        Assert.False(gate.IsSealedForShutdown);
    }

    [Fact]
    public async Task CrossedHelloAdmit_CompletesOnceOnBothSides()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(context, MeshName);
        await using var right = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var leftRid = RoutingId.From($"crossed-left-{suffix}");
        var rightRid = RoutingId.From($"crossed-right-{suffix}");
        var leftEndpoint = AllocateTcpEndpoint();
        var rightEndpoint = AllocateTcpEndpoint();

        left.SetRoutingId(leftRid);
        left.SetBind(leftEndpoint);
        left.AddChannel(MeshName);
        left.ConnectPeer(rightEndpoint, rightRid);
        right.SetRoutingId(rightRid);
        right.SetBind(rightEndpoint);
        right.AddChannel(MeshName);
        right.ConnectPeer(leftEndpoint, leftRid);
        using var leftMonitor = left.OpenMonitor();
        using var rightMonitor = right.OpenMonitor();

        // Both sides connect and announce, so each Hello is accepted and the
        // Admit that answers it arrives after the receiver already admitted
        // the same descriptor (the ZoneWorld E5 restart shape).
        left.Start();
        right.Start();
        await WaitUntilAsync(() =>
            left.Status().AdmittedPeerCount == 1
            && right.Status().AdmittedPeerCount == 1);
        // Let the crossed Admit replies and two admission retry intervals pass.
        await Task.Delay(TimeSpan.FromMilliseconds(1200));

        foreach (var (node, monitor, peerRid) in new[]
                 {
                     (left, leftMonitor, rightRid),
                     (right, rightMonitor, leftRid)
                 })
        {
            var status = monitor.Status();
            Assert.Equal(1UL, status.PeerAdmitted);
            Assert.Equal(0UL, status.ProtocolErrors);
            Assert.Equal(MeshNodeState.Ready, node.Status().State);
            Assert.Equal(1U, node.Status().AdmittedPeerCount);
            var admitted = Assert.Single(
                node.Peers(),
                peer => peer.RoutingId == peerRid);
            Assert.Equal(MeshPeerState.Admitted, admitted.State);
        }
    }

    [Fact]
    public async Task IdempotentAdmit_CompletesWithoutReadmittingOrResettingTheEpoch()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var endpoint = AllocateTcpEndpoint();
        node.SetRoutingId(RoutingId.From($"idempotent-node-{suffix}"));
        node.SetBind(endpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();

        var peerRid = RoutingId.From($"idempotent-peer-{suffix}");
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(peerRid);
        peer.Connect(endpoint);
        byte[] Descriptor(ServiceWireConstants.Command command) =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                command,
                MeshName,
                $"inproc://idempotent-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server);

        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello));
        await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
        await ReceiveAdmitAsync(peer);
        var admitted = Assert.Single(node.Peers());
        Assert.Equal(1UL, monitor.Status().PeerAdmitted);

        // The same descriptor again, first as Admit then as Hello: both are
        // idempotent completions. The Hello still gets its Admit reply; the
        // peer is neither re-admitted nor moved to a new epoch.
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Admit));
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello));
        await ReceiveAdmitAsync(peer);

        var status = monitor.Status();
        Assert.Equal(1UL, status.PeerAdmitted);
        Assert.Equal(0UL, status.PeerRejected);
        Assert.Equal(0UL, status.ProtocolErrors);
        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        var current = Assert.Single(node.Peers());
        Assert.Equal(MeshPeerState.Admitted, current.State);
        Assert.Equal(admitted.LifecycleGeneration, current.LifecycleGeneration);
        Assert.Equal(admitted.DescriptorRevision, current.DescriptorRevision);
        Assert.Equal(admitted.LastChangedMs, current.LastChangedMs);
    }

    [Fact]
    public async Task SealedNode_StartsNoPeerAdmissionAfterPeerLoss_AndKeepsDraining()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var gate = new ZLinkDrainAdmissionGate();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"sealed-peer-{suffix}");
        var peerEndpoint = AllocateTcpEndpoint();
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        node.SetRoutingId(RoutingId.From($"sealed-node-{suffix}"));
        node.SetBind(AllocateTcpEndpoint());
        node.AddChannel(MeshName);
        node.ConnectPeer(peerEndpoint, peerRid);

        await using (var peer = StartPeer(context, peerRid, peerEndpoint))
        {
            node.Start();
            await WaitUntilAsync(() =>
                node.Status().AdmittedPeerCount == 1
                && peer.Status().AdmittedPeerCount == 1);

            // ZLinkFrameworkRuntime shutdown order: seal host admission, then
            // publish Draining. The admitted peer still receives that Update.
            gate.ClaimShutdown();
            node.PublishDraining();
            await WaitUntilAsync(() => peer.Peers().Any(
                remote => remote.RoutingId == node.RoutingId
                          && remote.State == MeshPeerState.Draining));
        }

        // Peer loss demotes the outbound intent to a reconnecting epoch but
        // never moves the sealed node back before Draining.
        await WaitUntilAsync(() =>
            node.Status().AdmittedPeerCount == 0
            && node.Peers().All(remote => remote.State == MeshPeerState.Connecting));
        Assert.Equal(MeshNodeState.Draining, node.Status().State);

        // The peer restarts at the same endpoint. Core reconnects the pipe,
        // but the sealed node submits no Hello, so the restarted peer never
        // learns about it.
        await using (var restarted = StartPeer(context, peerRid, peerEndpoint))
        {
            await Task.Delay(TimeSpan.FromSeconds(2));
            Assert.Equal(0U, restarted.Status().AdmittedPeerCount);
            Assert.Empty(restarted.Peers());
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            Assert.Equal(MeshNodeState.Draining, node.Status().State);
            Assert.Equal(
                MeshPeerState.Connecting,
                Assert.Single(node.Peers()).State);

            // Drain completes: the node stops without waiting on the withheld
            // admission.
            await node.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(10));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task UnsealedNode_ReadmitsRestartedPeer(bool relocationDraining)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var gate = new ZLinkDrainAdmissionGate();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"unsealed-peer-{suffix}");
        var peerEndpoint = AllocateTcpEndpoint();
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        node.SetRoutingId(RoutingId.From($"unsealed-node-{suffix}"));
        node.SetBind(AllocateTcpEndpoint());
        node.AddChannel(MeshName);
        node.ConnectPeer(peerEndpoint, peerRid);

        await using (var peer = StartPeer(context, peerRid, peerEndpoint))
        {
            node.Start();
            await WaitUntilAsync(() =>
                node.Status().AdmittedPeerCount == 1
                && peer.Status().AdmittedPeerCount == 1);
            if (relocationDraining)
            {
                // Relocate publishes Draining without sealing the host: the
                // node keeps admitting peers.
                node.PublishDraining();
                await WaitUntilAsync(() => peer.Peers().Any(
                    remote => remote.RoutingId == node.RoutingId
                              && remote.State == MeshPeerState.Draining));
            }
        }

        await WaitUntilAsync(() =>
            node.Status().AdmittedPeerCount == 0
            && node.Peers().All(remote => remote.State == MeshPeerState.Connecting));

        await using var restarted = StartPeer(context, peerRid, peerEndpoint);
        await WaitUntilAsync(
            () => node.Status().AdmittedPeerCount == 1
                  && restarted.Status().AdmittedPeerCount == 1,
            TimeSpan.FromSeconds(15));
        Assert.False(gate.IsSealedForShutdown);
        Assert.Equal(
            relocationDraining ? MeshNodeState.Draining : MeshNodeState.Ready,
            node.Status().State);
        Assert.Equal(
            relocationDraining ? MeshPeerState.Draining : MeshPeerState.Admitted,
            Assert.Single(restarted.Peers()).State);
    }

    private static ZLinkManagedMeshNode StartPeer(
        IContext context,
        RoutingId routingId,
        string endpoint)
    {
        var peer = new ZLinkManagedMeshNode(context, MeshName);
        peer.SetRoutingId(routingId);
        peer.SetBind(endpoint);
        peer.AddChannel(MeshName);
        peer.Start();
        return peer;
    }

    private static async Task SendAsync(IDealerSocket socket, byte[] head)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (true)
        {
            try
            {
                using var message = Message.From(head);
                await socket.Send().Message(message).Async(CancellationToken.None);
                return;
            }
            catch (ZlinkSubmitException) when (DateTime.UtcNow < deadline)
            {
                await Task.Delay(10);
            }
        }
    }

    // Skips liveness probes; returns once a route Admit arrives.
    private static async Task ReceiveAdmitAsync(IDealerSocket socket)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            using var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
            {
                if (ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _)
                    && command == ServiceWireConstants.Command.Admit)
                    return;
                continue;
            }
            await Task.Delay(10);
        }
        throw new TimeoutException("The route Admit reply was not received.");
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition,
        TimeSpan? timeout = null)
    {
        var deadline = DateTime.UtcNow + (timeout ?? TimeSpan.FromSeconds(5));
        while (!condition())
        {
            if (DateTime.UtcNow >= deadline)
                throw new TimeoutException("The managed MeshNode condition was not reached.");
            await Task.Delay(10);
        }
    }

    private static string AllocateTcpEndpoint()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = Assert.IsType<IPEndPoint>(listener.LocalEndpoint);
        return $"tcp://127.0.0.1:{endpoint.Port}";
    }
}
