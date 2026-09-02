using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_router_multiple_dealers
{
    [Theory]
    [InlineData("tcp")]
    [InlineData("ipc")]
    [InlineData("inproc")]
    public async Task router_multiple_dealers(string transport)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported(transport))
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer1 = ctx.CreateDealerSocket();
        using var dealer2 = ctx.CreateDealerSocket();

        dealer1.SetRoutingId(CoreTestSupport.RoutingIdUtf8("D1"));
        dealer2.SetRoutingId(CoreTestSupport.RoutingIdUtf8("D2"));

        string endpoint = CoreTestSupport.NewEndpoint(transport,
            "router-multi-dealer");
        router.Bind(endpoint);
        dealer1.Connect(endpoint);
        dealer2.Connect(endpoint);
        Thread.Sleep(300);

        CoreTestSupport.SendAsyncWithTimeout(dealer1, "from_dealer1"u8,
            2000);
        CoreTestSupport.SendAsyncWithTimeout(dealer2, "from_dealer2"u8,
            2000);

        var received = new Dictionary<string, string>();
        for (int i = 0; i < 2; i++)
        {
            (string id, string payload) =
                CoreTestSupport.ReceiveRoutedUtf8WithTimeout(router, 2000);
            received[id] = payload;
        }

        Assert.Equal(2, received.Count);
        Assert.Contains("from_dealer1", received.Values);
        Assert.Contains("from_dealer2", received.Values);

        string dealer1RoutingId = received.First(kvp => kvp.Value == "from_dealer1").Key;
        string dealer2RoutingId = received.First(kvp => kvp.Value == "from_dealer2").Key;

        using Message reply1 = Message.From("reply_to_d1");
        await router.Send(
                RoutingId.From(Encoding.UTF8.GetBytes(dealer1RoutingId)))
            .Message(reply1).Async();

        using Message reply2 = Message.From("reply_to_d2");
        await router.Send(
                RoutingId.From(Encoding.UTF8.GetBytes(dealer2RoutingId)))
            .Message(reply2).Async();

        Assert.Equal("reply_to_d1", CoreTestSupport.ReceiveUtf8WithTimeout(dealer1,
            2000));
        Assert.Equal("reply_to_d2", CoreTestSupport.ReceiveUtf8WithTimeout(dealer2,
            2000));
    }

    [Fact]
    public void received_send_single_message_replies_to_routed_source()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();

        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("D1"));
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "received-send-routed");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        CoreTestSupport.SendAsyncWithTimeout(dealer, "ping"u8, 2000);

        using var received = Received.Create();
        router.Recv(received);
        Assert.Equal("ping", Encoding.UTF8.GetString(
            received.SinglePartOrThrow().AsReadOnlySpan()));

        using Message reply = Message.From("pong");
        received.Send().Message(reply).Submit();

        Assert.Equal("pong", CoreTestSupport.ReceiveUtf8WithTimeout(dealer,
            2000));
    }

    [Fact]
    public async Task async_received_send_transfers_multipart_and_reuses_envelope()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();

        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("D1"));
        router.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "received-send-async-multipart");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message payload = Message.From("first");
        using Message tail = Message.Allocate(0);
        await dealer.Send().Message(payload).Message(tail).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));

        using var received = Received.Create();
        Assert.True(router.Recv(received));
        IReadOnlyList<Message> forwardedParts = received.Parts;
        Assert.Collection(forwardedParts,
            part => Assert.Equal("first", part.GetString()),
            part => Assert.Equal(0, part.Size));

        Task reply = received.Send().Messages(forwardedParts).Async();
        Assert.All(forwardedParts, part =>
            Assert.Throws<ObjectDisposedException>(() => _ = part.Size));
        await reply.WaitAsync(TimeSpan.FromSeconds(2));

        using Received echoed = CoreTestSupport.ReceiveMessageWithTimeout(dealer,
            2000);
        Assert.Collection(echoed.Parts,
            part => Assert.Equal("first", part.GetString()),
            part => Assert.Equal(0, part.Size));

        using Message nextPayload = Message.From("second");
        using Message nextTail = Message.Allocate(0);
        await dealer.Send().Message(nextPayload).Message(nextTail).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(router.Recv(received));
        Assert.Collection(received.Parts,
            part => Assert.Equal("second", part.GetString()),
            part => Assert.Equal(0, part.Size));
    }

    [Fact]
    public async Task async_received_send_keeps_captured_routing_id_after_handover()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var first = ctx.CreateDealerSocket();
        using var replacement = ctx.CreateDealerSocket();

        RoutingId sourceRid = CoreTestSupport.RoutingIdUtf8("same-source");
        first.SetRoutingId(sourceRid);
        replacement.SetRoutingId(sourceRid);
        router.Options.Handover = true;
        router.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "received-send-exact-handover");
        router.Bind(endpoint);
        first.Connect(endpoint);
        Thread.Sleep(100);

        using Message firstPayload = Message.From("first");
        await first.Send().Message(firstPayload).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));
        using var captured = Received.Create();
        Assert.True(router.Recv(captured));
        SendOperation capturedSend = captured.Send();

        replacement.Connect(endpoint);
        Thread.Sleep(100);
        using Message replacementPayload = Message.From("replacement");
        await replacement.Send().Message(replacementPayload).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));
        using var replacementReceived = Received.Create();
        Assert.True(router.Recv(replacementReceived));
        Assert.Equal("replacement",
            replacementReceived.SinglePartOrThrow().GetString());

        // Reuse clears the mutable Received instance. The operation above must
        // retain the source route snapshot captured before the handover.
        Assert.False(router.Recv(captured, RecvFlags.DontWait));
        using Message exactReply = Message.From("first-only");
        await capturedSend.Message(exactReply).Async();

        using var firstUnexpected = Received.Create();
        Assert.False(first.Recv(firstUnexpected, RecvFlags.DontWait));
        using var delivered = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(
            () => replacement.Recv(delivered, RecvFlags.DontWait), 2000));
        Assert.Equal("first-only", delivered.SinglePartOrThrow().GetString());
    }

    [Fact]
    public async Task async_request_received_send_uses_captured_source_context()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "request-received-send-source");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("request");
        Task<IReadOnlyList<Message>> completion = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using var received = Received.Create();
        Assert.True(router.Recv(received));
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);

        using Message sideMessage = Message.From("source-send");
        await received.Send().Message(sideMessage).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("source-send",
            CoreTestSupport.ReceiveUtf8WithTimeout(dealer, 2000));

        using Message reply = Message.From("reply");
        received.Reply().Message(reply).Submit();
        IReadOnlyList<Message> response = await completion.WaitAsync(
            TimeSpan.FromSeconds(2));
        Assert.Equal("reply", Assert.Single(response).GetString());
        Zlink.MultipartClose(response);
    }

    [Fact]
    public async Task router_received_preserves_transport_pair_and_resets_on_reuse()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();

        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("pair-source"));
        router.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "router-received-transport-pair");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message first = Message.From("first");
        await dealer.Send().Message(first).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));

        using var received = Received.Create();
        Assert.True(router.Recv(received));
        Assert.Equal("first", received.SinglePartOrThrow().GetString());

        using Message next = Message.From("next");
        await dealer.Send().Message(next).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));
        Assert.True(CoreTestSupport.WaitUntil(
            () => router.Recv(received, RecvFlags.DontWait), 2000));
        Assert.Equal("next", received.SinglePartOrThrow().GetString());

        using Message multipartHead = Message.From("multipart");
        using Message multipartTail = Message.From("tail");
        await dealer.Send().Message(multipartHead).Message(multipartTail).Async()
            .WaitAsync(TimeSpan.FromSeconds(2));
        Assert.True(CoreTestSupport.WaitUntil(
            () => router.Recv(received, RecvFlags.DontWait), 2000));
        Assert.Collection(received.Parts,
            part => Assert.Equal("multipart", part.GetString()),
            part => Assert.Equal("tail", part.GetString()));

        Assert.False(router.Recv(received, RecvFlags.DontWait));
    }

    [Fact]
    public async Task routed_direct_send_and_recv_part_roundtrip()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();

        RoutingId dealerRid = CoreTestSupport.RoutingIdUtf8("D1");
        dealer.SetRoutingId(dealerRid);
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "routed-direct-part");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message outbound = Message.From("ping");
        await dealer.Send().Message(outbound).Async();

        using var inbound = Received.Create();
        Assert.True(router.Recv(inbound));
        RoutingId? sourceRid = inbound.RoutingId;
        Assert.True(sourceRid.HasValue);
        RoutingId actualSourceRid = sourceRid.GetValueOrDefault();
        Assert.Equal(dealerRid, actualSourceRid);
        Assert.Equal("ping", Encoding.UTF8.GetString(
            inbound.FirstPart().AsReadOnlySpan()));

        using Message reply = Message.From("pong");
        await router.Send(actualSourceRid).Message(reply).Async();

        using var dealerInbound = Received.Create();
        Assert.True(dealer.Recv(dealerInbound));
        Assert.Equal("pong", Encoding.UTF8.GetString(
            dealerInbound.FirstPart().AsReadOnlySpan()));
    }

}
