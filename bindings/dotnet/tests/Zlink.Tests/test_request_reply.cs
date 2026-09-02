using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_request_reply
{
    [Fact]
    public async Task request_sync_return_terminal_returns_reply()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "request-sync");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        CoreTestSupport.WaitReady(dealer);

        Task server = Task.Run(() => ReplyOnce(router, "sync-pong"));
        using Message request = Message.From("sync-ping");
        IReadOnlyList<Message> reply = dealer.Request().Message(request)
            .Timeout(TimeSpan.FromSeconds(2)).Submit();
        Assert.Equal("sync-pong", reply[0].GetString());
        Zlink.MultipartClose(reply);
        await server;
    }

    [Fact]
    public async Task request_async_terminal_delivers_reply()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "request-callback");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        CoreTestSupport.WaitReady(dealer);

        Task server = Task.Run(() => ReplyOnce(router, "callback-pong"));
        using Message request = Message.From("callback-ping");
        IReadOnlyList<Message> reply = await dealer.Request().Message(request)
            .Timeout(TimeSpan.FromSeconds(2)).Async();

        Assert.Equal("callback-pong", Assert.Single(reply).GetString());
        Zlink.MultipartClose(reply);
        await server;
    }

    [Fact]
    public void router_poller_can_own_receive_and_completion_after_bind()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "router-poller-receive-completion");

        router.Bind(endpoint);

        poller.Add(
            router,
            PollEventFlags.PollIn | PollEventFlags.PollCompletion,
            1);
        Assert.Equal(1, poller.Size);
    }

    [Fact]
    public async Task request_dealer_router_roundtrip()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var routerSocket = ctx.CreateRouterSocket();
        using var dealerSocket = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc", "request-reply");
        routerSocket.Bind(endpoint);
        dealerSocket.Connect(endpoint);
        CoreTestSupport.WaitReady(dealerSocket);

        using var handled = new ManualResetEventSlim(false);
        Task serverTask = Task.Run(() =>
        {
            var received = Received.Create();
            routerSocket.Recv(received);
            try
            {
                Assert.True(received.ReplyToken is not null);
                Assert.Equal("ping", received.Parts[0].GetString());
                using Message reply = Message.From("pong");
                routerSocket.Reply(
                    received.RoutingId ?? throw new InvalidOperationException(
                        "missing routing id"), received.ReplyToken!)
                    .Message(reply).Submit();
                handled.Set();
            }
            finally
            {
                foreach (Message part in received.Parts) part.Dispose();
            }
        });

        using Message request = Message.From("ping");
        IReadOnlyList<Message> reply = await dealerSocket.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        try
        {
            Assert.Equal("pong", reply[0].GetString());
        }
        finally
        {
            foreach (Message part in reply)
                part.Dispose();
        }

        Assert.True(handled.Wait(2000));
        await serverTask;
    }

    [Fact]
    public async Task routed_router_request_preserves_request_envelope_kind()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc",
            "router-request-envelope");
        RoutingId serverRid = CoreTestSupport.RoutingIdUtf8("router-server");
        server.SetRoutingId(serverRid);
        client.SetRoutingId(CoreTestSupport.RoutingIdUtf8("router-client"));
        client.Options.SetConnectRoutingId(serverRid);
        server.Bind(endpoint);
        client.Connect(endpoint);
        CoreTestSupport.WaitReady(client);

        using Message request = Message.From("ping");
        Task<IReadOnlyList<Message>> completion = client.Request(serverRid)
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();

        using Received received = RecvWithRetry(server);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.ReplyToken is not null);
        Assert.Equal("ping", received.Parts[0].GetString());
        ReplyOperation detachedReply = received.Reply();
        received.Dispose();

        using Message reply = Message.From("pong");
        detachedReply.Message(reply).Submit();
        var duplicate = Assert.Throws<ZlinkConfigException>(
            () => detachedReply.Message(reply));
        Assert.Equal(
            ZlinkConfigException.ErrorCode.InvalidState,
            duplicate.Result);

        IReadOnlyList<Message> replyParts =
            await completion.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("pong", replyParts[0].GetString());
        Zlink.MultipartClose(replyParts);
    }

    [Fact]
    public async Task dealer_receives_unsolicited_message_after_request_reply()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("request-client"));
        string endpoint = CoreTestSupport.NewEndpoint(
            "tcp",
            "request-then-unsolicited");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        CoreTestSupport.WaitReady(dealer);

        using Message request = Message.From("hello");
        Task<IReadOnlyList<Message>> replyTask = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using Received inbound = Received.Create();
        Assert.True(router.Recv(inbound));
        RoutingId sourceRid = inbound.RoutingId
            ?? throw new InvalidOperationException("missing routing id");
        ReplyToken replyToken = inbound.ReplyToken
            ?? throw new InvalidOperationException("missing reply token");
        using Message admitted = Message.From("admitted");
        router.Reply(sourceRid, replyToken)
            .Message(admitted)
            .Submit();
        IReadOnlyList<Message> reply = await replyTask;
        foreach (Message part in reply)
            part.Dispose();

        using Message update = Message.From("unsolicited");
        await router.Send(sourceRid).Message(update).Async();
        string unsolicited =
            CoreTestSupport.ReceiveUtf8WithTimeout(dealer, 2000);
        Assert.Equal("unsolicited", unsolicited);
        dealer.Disconnect(endpoint);
    }

    [Fact]
    public async Task dealer_receives_unsolicited_message_after_dontwait_polled_request_reply()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("dontwait-probe-client"));
        string endpoint = CoreTestSupport.NewEndpoint(
            "tcp",
            "request-then-unsolicited-dontwait");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        CoreTestSupport.WaitReady(dealer);

        using Message request = Message.From("hello");
        Task<IReadOnlyList<Message>> replyTask = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();

        // Regression guard for BLK-004: the ROUTER side of a ClientServer
        // admission handshake polls its initial recv with RecvFlags.DontWait
        // (see ZLinkClientServerClientRuntime/ZLinkBackendRouterSocketWrapper),
        // not a blocking Recv. A DEALER that completed exactly one async
        // request must still surface a later unsolicited raw record through
        // Recv() when the peer's request was received via DontWait polling.
        Received inbound = RecvWithRetry(router);
        RoutingId sourceRid = inbound.RoutingId
            ?? throw new InvalidOperationException("missing routing id");
        ReplyToken replyToken = inbound.ReplyToken
            ?? throw new InvalidOperationException("missing reply token");
        foreach (Message part in inbound.Parts) part.Dispose();

        using Message admitted = Message.From("admitted");
        router.Reply(sourceRid, replyToken)
            .Message(admitted)
            .Submit();
        IReadOnlyList<Message> reply = await replyTask;
        foreach (Message part in reply)
            part.Dispose();

        using Message update = Message.From("unsolicited");
        await router.Send(sourceRid).Message(update).Async();

        using Received delivered = RecvWithRetry(dealer);
        Assert.Equal("unsolicited", delivered.Parts[0].GetString());
        dealer.Disconnect(endpoint);
    }


    [Fact]
    public async Task request_router_preserves_data_receive_surface()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var routerSocket = ctx.CreateRouterSocket();
        using var dealerSocket = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "request-reply-data");
        routerSocket.Bind(endpoint);
        dealerSocket.Connect(endpoint);
        CoreTestSupport.WaitReady(dealerSocket);

        using Message payload = Message.From("plain-data");
        await dealerSocket.Send().Message(payload).Async();

        var received = Received.Create();
        routerSocket.Recv(received);
        try
        {
            string routedPayload = received.Parts.Count == 0
                ? string.Empty
                : received.Parts[received.Parts.Count - 1].GetString();
            Assert.NotNull(routedPayload);
        }
        finally
        {
            foreach (Message part in received.Parts)
                part.Dispose();
        }
    }

    [Fact]
    public async Task request_async_transfers_reply_message_ownership_to_application()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var routerSocket = ctx.CreateRouterSocket();
        using var dealerSocket = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "request-reply-callback-owned");
        routerSocket.Bind(endpoint);
        dealerSocket.Connect(endpoint);
        CoreTestSupport.WaitReady(dealerSocket);

        using var handled = new ManualResetEventSlim(false);
        Message? owned = null;

        Task serverTask = Task.Run(() =>
        {
            var received = Received.Create();
            routerSocket.Recv(received);
            try
            {
                using Message reply = Message.From("pong-owned");
                routerSocket.Reply(
                    received.RoutingId ?? throw new InvalidOperationException(
                        "missing routing id"), received.ReplyToken
                        ?? throw new InvalidOperationException(
                            "missing reply token"))
                    .Message(reply).Submit();
                handled.Set();
            }
            finally
            {
                foreach (Message part in received.Parts) part.Dispose();
            }
        });

        using Message request = Message.From("ping-owned");
        IReadOnlyList<Message> reply = await dealerSocket.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async()
            .WaitAsync(TimeSpan.FromSeconds(10));
        Assert.Single(reply);
        owned = reply[0];

        Assert.True(handled.Wait(10000));
        Assert.NotNull(owned);
        Assert.Equal("pong-owned", owned!.GetString());
        owned.Dispose();
        Assert.Throws<ObjectDisposedException>(() => _ = owned.Size);
        await serverTask;
    }

    private static Received RecvWithRetry(IRouterSocket socket)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
                return received;

            received.Dispose();
            Thread.Sleep(1);
        }

        throw new TimeoutException("Timed out waiting for router message.");
    }

    private static void ReplyOnce(IRouterSocket router, string payload)
    {
        using Received received = RecvWithRetry(router);
        using Message reply = Message.From(payload);
        router.Reply(received.RoutingId!.Value, received.ReplyToken!)
            .Message(reply).Submit();
    }

    private static Received RecvWithRetry(IDealerSocket socket)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
                return received;

            received.Dispose();
            Thread.Sleep(1);
        }

        throw new TimeoutException("Timed out waiting for dealer message.");
    }

    private static Received RecvWithRetry(params IDealerSocket[] sockets)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            foreach (var socket in sockets)
            {
                var received = Received.Create();
                if (socket.Recv(received, RecvFlags.DontWait))
                    return received;

                received.Dispose();
            }

            Thread.Sleep(1);
        }

        throw new TimeoutException("Timed out waiting for dealer message.");
    }
}
