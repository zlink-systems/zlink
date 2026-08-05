using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SubmitAdmissionRuntimeTests
{
    [Fact]
    public void RouteSendCall_ValidatesMessageBeforeSubmitCancellationCanRun()
    {
        Assert.Throws<InvalidOperationException>(() =>
            new ZLinkRouteSendCall<object>(
                runtime: null!,
                meshName: "mesh",
                targetNodeRid: RoutingId.From("target"),
                message: null!));
    }

    [Fact]
    public void ManualPeerTombstone_ReassignmentKeepsOnlyLatestLogicalIdentity()
    {
        var connections = new ZLinkSpotPeerConnectionSet();
        var first = RoutingId.From("first");
        var second = RoutingId.From("second");

        connections.RetainManualPeerRid("tcp://127.0.0.1:7101", first);
        connections.RetainManualPeerRid("tcp://127.0.0.1:7101", second);

        Assert.False(connections.HasRetainedManualPeer(first));
        Assert.True(connections.HasRetainedManualPeer(second));
        Assert.False(new ZLinkSpotPeerConnectionSet().HasRetainedManualPeer(second));
    }

    [Fact]
    public void RouteSendFastFailure_DisposesEveryMessageBeforeHandoff()
    {
        var disposed = new List<Message>();
        for (var attempt = 0; attempt < 100; attempt++)
        {
            var parts = ZLinkMessageParts.Create(
                Message.From(new byte[] { 1, 2, 3 }),
                Message.From(new byte[] { 4, 5, 6 }));
            disposed.Add(parts[0]);
            disposed.Add(parts[1]);

            ZLinkRouteSendCall<object>.DisposeBeforeHandoff(parts);
        }

        Assert.Equal(200, disposed.Count);
        Assert.All(disposed, message =>
            Assert.Throws<ObjectDisposedException>(() => _ = message.Size));
    }
}
