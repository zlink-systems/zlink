using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Fact]
    public async Task NativeSessionBind_IntentRemovalEndsWithoutWaitingForDeadline()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var deadlineTime = new ControllableTimeProvider();
        await using var source = NewNode(
            context,
            "bind-intent-source",
            deadlineTimeProvider: deadlineTime
        );
        var target = RoutingId.From("bind-intent-target");
        var endpoint = $"inproc://bind-intent-{Guid.NewGuid():N}";
        source.SetPeerExpectation(target, endpoint, ZLinkServiceSecurityIdentity.Plaintext, 7);
        source.Start();
        using var payload = Message.From("bind-operation"u8);
        var request = source
            .RequestToNodeDirectAsync(
                target,
                [payload],
                SendFlags.None,
                default,
                TimeSpan.FromSeconds(2),
                CancellationToken.None,
                durable: true
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        source.RemovePeerExpectation(target, endpoint);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            request.WaitAsync(TimeSpan.FromSeconds(1))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public async Task NativeDurablePending_ImmediateIntentRemovalPublishesTerminal()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var deadlineTime = new ControllableTimeProvider();
        await using var source = NewNode(
            context,
            "bind-pending-source",
            deadlineTimeProvider: deadlineTime
        );
        var target = RoutingId.From("bind-pending-target");
        var endpoint = $"inproc://bind-pending-{Guid.NewGuid():N}";
        source.SetPeerExpectation(target, endpoint, ZLinkServiceSecurityIdentity.Plaintext, 7);
        source.Start();
        var reservation = new ObjectReservationFence(
            "bind-reservation",
            "bind-store",
            11,
            13,
            target,
            7,
            "bind-owner",
            17,
            1
        );
        var timeout = TimeSpan.FromSeconds(2);
        Assert.Equal(
            SubmitResult.Ok,
            source.CreateActorRemote(
                target,
                "pending-actor",
                "Sample.Actor",
                reservation,
                checked((ulong)deadlineTime.GetUtcNow().Add(timeout).ToUnixTimeMilliseconds()),
                out var operation,
                timeout
            )
        );
        source.RemovePeerExpectation(target, endpoint);
        var completion = Assert.Single(
            DrainRecords(source),
            record => record.OperationId == operation && record.Kind == MeshRecordKind.Completion
        );
        Assert.Equal((int)RequestResult.NotConnected, completion.TerminalResult);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task NativeSessionBind_WaitsForAdmissionAndConsumesOneTerminal(bool rejected)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "bind-source");
        await using var target = NewNode(context, "bind-target");
        var suffix = Guid.NewGuid().ToString("N");
        source.SetBind($"inproc://bind-source-{suffix}");
        var endpoint = $"inproc://bind-target-{suffix}";
        target.SetBind(endpoint);
        source.Start();
        target.Start();
        using var payload = Message.From("bind-operation"u8);
        var request = source
            .RequestToNodeDirectAsync(
                target.RoutingId,
                [payload],
                SendFlags.None,
                default,
                TimeSpan.FromSeconds(2),
                CancellationToken.None,
                durable: true
            )
            .AsTask();
        // Request execution reaches the initial missing-peer admission before
        // returning its first incomplete await. The one-shot path is terminal here.
        Assert.False(request.IsCompleted);
        source.ConnectPeer(endpoint, target.RoutingId);
        using var ready = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            target.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
            return ready.Count > 0;
        });
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(MeshRecordKind.NodeRequest, received[0].Kind);
        if (rejected)
        {
            var header = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request,
                "mesh",
                "bind"
            ) with
            {
                Kind = ZLinkMessageKind.Error,
                ErrorCode = "unavailable",
                ErrorMessage = "terminal rejection",
            };
            using var terminal = ZLinkEnvelopeCodec.EncodeHeader(header);
            Assert.Equal(SubmitResult.Ok, received[0].Reply([terminal]));
            var reply = await request;
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
                ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<object>(
                    reply,
                    "empty",
                    "bind rejected",
                    new ZLinkFrameworkRegistration().Codecs
                )
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        }
        else
        {
            using var terminal = Message.From("bound"u8);
            Assert.Equal(SubmitResult.Ok, received[0].Reply([terminal]));
            using var reply = await request;
            Assert.Equal(
                terminal.AsReadOnlyMemory().ToArray(),
                reply.ApplicationPayloadView!.GetSpan(0).ToArray()
            );
        }
        Assert.False(claim.Receive(received, RecvFlags.DontWait));
    }
}
