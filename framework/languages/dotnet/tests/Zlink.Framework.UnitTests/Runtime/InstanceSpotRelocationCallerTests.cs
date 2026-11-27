using Zlink.Framework.Runtime.Service;
using Wire = Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Fact]
    public async Task InstanceSpotRelocationCompletesTheOriginalReadyCaller()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = NewNode(context, "ready-relocation-caller");
        await using var owner = NewNode(context, "ready-relocation-owner");
        await using var target = NewNode(context, "ready-relocation-target");
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://ready-relocation-caller-{suffix}";
        var ownerEndpoint = $"inproc://ready-relocation-owner-{suffix}";
        var targetEndpoint = $"inproc://ready-relocation-target-{suffix}";
        caller.SetBind(callerEndpoint);
        owner.SetBind(ownerEndpoint);
        target.SetBind(targetEndpoint);
        caller.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.SetLocalOwnerLeaseGeneration(11);
        owner.SetLocalOwnerLeaseGeneration(23);
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner",
            11,
            caller.RoutingId,
            caller.Status().LifecycleGeneration
        );
        var ownerGeneration = owner.Status().LifecycleGeneration;
        caller.SetLocalRequestSourceFence(source);
        owner.SetLocalRequestSourceFence(new("spot-owner", 23, owner.RoutingId, ownerGeneration));
        caller.SetRelocationReplyRelayTarget(
            new ZLinkRelocationReplyTarget(
                new ZLinkBackendSpotNodeWrapper(caller).TryCompleteRelocationReply
            )
        );
        caller.Start();
        owner.Start();
        target.Start();
        await WaitUntilAsync(() =>
            caller.Status().AdmittedPeerCount == 2
            && owner.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1
        );

        const string spotId = "ready-room";
        _ = owner.GetOrCreateReservedSpot(spotId, 17, 19, out _);
        caller.ObserveSpotAuthority(owner.RoutingId, spotId, 17, ownerGeneration, 19, 23);
        var readyRoute = new Wire.InstanceRouteV1Case0(
            Wire.InstanceRouteKind.Ready,
            new(owner.RoutingId),
            new(ownerGeneration),
            new(spotId),
            new(new(17), new("spot-owner"), new(19), new(23), new("ready-version")),
            Wire.Bool8.True
        );

        using var request = Message.From(new byte[] { 3 });
        var response = caller
            .RequestToSpotDirectAsync(
                "caller-spot",
                owner.RoutingId,
                spotId,
                17,
                [request],
                SendFlags.None,
                default,
                TimeSpan.FromSeconds(5),
                CancellationToken.None,
                readyRoute
            )
            .AsTask();
        await WaitUntilAsync(() => owner.Status().PendingApplicationMessages != 0);
        var original = Assert.Single(
            DrainRecords(owner),
            record => record.Kind == MeshRecordKind.SpotRequest
        );
        Assert.Equal(readyRoute, original.ReadyInstanceRoute);
        var targetGeneration = target.Status().LifecycleGeneration;
        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            original.OperationId,
            original.ReplyRouteId,
            new(1, 2),
            1,
            new("new-owner", 29, target.RoutingId, targetGeneration, "relocated-version"),
            1,
            1,
            (uint)RequestResult.Ok,
            0
        );
        using var header = Message.From(new byte[] { 4 });
        using var body = Message.From(new byte[] { 5 });

        var terminal = await target.RelayRelocationReplyAsync(
            caller.RoutingId,
            relay,
            source,
            [header, body],
            TimeSpan.FromSeconds(3),
            CancellationToken.None
        );
        Assert.Equal((byte)ZLinkRelocationReplyCompletionState.TerminalReceived, terminal.Status);
        Assert.Equal(source, terminal.RequestSource);
        using var reply = await response;
        Assert.Equal(2, reply.Parts.Count);
        Assert.Equal(new byte[] { 4 }, reply.Parts[0].ToArray());
        Assert.Equal(new byte[] { 5 }, reply.Parts[1].ToArray());

        var duplicate = await target.RelayRelocationReplyAsync(
            caller.RoutingId,
            relay,
            source,
            [header, body],
            TimeSpan.FromSeconds(3),
            CancellationToken.None
        );
        Assert.Equal((byte)ZLinkRelocationReplyCompletionState.AlreadyTerminal, duplicate.Status);
    }
}
