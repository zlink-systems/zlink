using System.Linq;
using Xunit;

namespace Systems.Zlink.Tests;

// Core ROUTER §10.1 projection: selected-route snapshot, POLLROUTE readiness
// and the route generation of a received ROUTER record. The scenario mirrors
// core/tests/integration/test_router_selected_route_contract.cpp.
public sealed class test_router_selected_route
{
    private const int WaitMs = 3000;

    [Fact]
    public void snapshot_pollroute_and_recv_generation_follow_selected_route()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var client = Router(ctx, "C");
        using var oldServer = Router(ctx, "S");
        using var newServer = Router(ctx, "S");
        oldServer.Bind("tcp://127.0.0.1:*");
        newServer.Bind("tcp://127.0.0.1:*");
        var oldEndpoint = oldServer.Options.LastEndpoint;
        var newEndpoint = newServer.Options.LastEndpoint;

        Assert.Empty(client.RoutesSnapshot());
        using var poller = Zlink.CreatePoller();
        poller.Add(client, PollEventFlags.PollRoute, 7);

        client.Options.SetConnectRoutingId(CoreTestSupport.RoutingIdUtf8("S"));
        client.Connect(oldEndpoint);
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(WaitMs)));
        Assert.Equal((nuint)7, events[0].Slot);
        Assert.True((events[0].Revents & PollEventFlags.PollRoute) != 0);

        var first = WaitGeneration(client, "S", previous: 0);
        Assert.NotEqual(0UL, first);
        // A snapshot that reflects every change clears the level readiness.
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));

        WaitGeneration(oldServer, "C", previous: 0);
        Send(oldServer, "C", "selected");
        using (var received = CoreTestSupport.ReceiveMessageWithTimeout(client, WaitMs))
        {
            Assert.Equal(CoreTestSupport.RoutingIdUtf8("S"), received.RoutingId);
            Assert.Equal(first, received.RouteGeneration);
        }

        // A same-direction reconnect with the same RID takes over the selected
        // route (HANDOVER): the generation changes and POLLROUTE is reported.
        client.Options.SetConnectRoutingId(CoreTestSupport.RoutingIdUtf8("S"));
        client.Connect(newEndpoint);
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(WaitMs)));
        Assert.True((events[0].Revents & PollEventFlags.PollRoute) != 0);
        var second = WaitGeneration(client, "S", previous: first);
        Assert.NotEqual(first, second);

        WaitGeneration(newServer, "C", previous: 0);
        Send(newServer, "C", "replacement");
        using (var received = CoreTestSupport.ReceiveMessageWithTimeout(client, WaitMs))
            Assert.Equal(second, received.RouteGeneration);
    }

    [Fact]
    public void route_generation_is_zero_for_non_router_records_and_pollroute_is_router_only()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = Router(ctx, "R");
        using var dealer = ctx.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "selected-route-dealer");
        router.Bind(endpoint);
        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("D"));
        dealer.Connect(endpoint);
        WaitGeneration(router, "D", previous: 0);

        Send(router, "D", "to-dealer");
        using var received = CoreTestSupport.ReceiveMessageWithTimeout(dealer, WaitMs);
        Assert.Equal(0UL, received.RouteGeneration);

        using var poller = Zlink.CreatePoller();
        Assert.ThrowsAny<ZlinkException>(() =>
            poller.Add(dealer, PollEventFlags.PollRoute, 1));
    }

    private static IRouterSocket Router(IContext ctx, string rid)
    {
        var socket = ctx.CreateRouterSocket();
        socket.SetRoutingId(CoreTestSupport.RoutingIdUtf8(rid));
        socket.Options.Linger = TimeSpan.Zero;
        socket.Options.Handover = true;
        return socket;
    }

    private static ulong WaitGeneration(IRouterSocket socket, string rid, ulong previous)
    {
        var expected = CoreTestSupport.RoutingIdUtf8(rid);
        ulong generation = 0;
        Assert.True(CoreTestSupport.WaitUntil(() =>
        {
            var rows = socket.RoutesSnapshot().Where(row => row.RoutingId == expected).ToArray();
            Assert.True(rows.Length <= 1);
            if (rows.Length == 0 || rows[0].RouteGeneration == previous)
            {
                // Like the Core contract test, wait on POLLROUTE between
                // snapshots; polling also lets the socket apply pipe changes.
                Span<PollEventFlags> revents = stackalloc PollEventFlags[1];
                ZlinkPoll.Poll([socket], [PollEventFlags.PollRoute], revents, 10);
                return false;
            }
            Assert.NotEqual(0UL, rows[0].RouteGeneration);
            generation = rows[0].RouteGeneration;
            return true;
        }, WaitMs), $"selected route for {rid} did not change");
        return generation;
    }

    private static void Send(IRouterSocket socket, string rid, string payload)
    {
        using var message = Message.From(payload);
        socket.Send(CoreTestSupport.RoutingIdUtf8(rid)).Message(message).Submit();
    }
}
