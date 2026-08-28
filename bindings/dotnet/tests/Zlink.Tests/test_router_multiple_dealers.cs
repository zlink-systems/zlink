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
        received.Send().Message(reply).Submit(SendFlags.None);

        Assert.Equal("pong", CoreTestSupport.ReceiveUtf8WithTimeout(dealer,
            2000));
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
