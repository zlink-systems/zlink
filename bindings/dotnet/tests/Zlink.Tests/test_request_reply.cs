using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Systems.Zlink.Tests;

    public sealed class test_request_reply
    {
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

        router.OnSendReady(() => { });
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
        Thread.Sleep(50);

        using var handled = new ManualResetEventSlim(false);
        Task serverTask = Task.Run(() =>
        {
            var received = Received.Create();
            routerSocket.Recv(received);
            try
            {
                Assert.True(received.RequestSeq.HasValue);
                Assert.NotEqual(0UL, received.RequestSeq.Value);
                Assert.Equal("ping", received.Parts[0].GetString());
                using Message reply = Message.From("pong");
                routerSocket.Reply(
                    received.RoutingId ?? throw new InvalidOperationException(
                        "missing routing id"), received.RequestSeq.Value)
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
        Thread.Sleep(50);

        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using Message request = Message.From("ping");
        Assert.True(client.Request(serverRid)
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Submit((result, parts) =>
            {
                if (result == RequestResult.Ok)
                    completion.TrySetResult(parts);
                else
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
            }));

        using Received received = RecvWithRetry(server);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);
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
            await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("pong", replyParts[0].GetString());
        Zlink.MultipartClose(replyParts);
    }

    [Fact]
    public async Task router_completion_control_progresses_without_application_recv()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateRouterSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "completion-control");
        RoutingId serverRid = CoreTestSupport.RoutingIdUtf8("control-server");
        RoutingId clientRid = CoreTestSupport.RoutingIdUtf8("control-client");
        server.SetRoutingId(serverRid);
        client.SetRoutingId(clientRid);
        client.Options.SetConnectRoutingId(serverRid);

        var delivered = new TaskCompletionSource<(
            RoutingId Source, IReadOnlyList<Message> Parts)>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var invokedRegistration = -1;
        for (var registration = 0; registration < 16; registration++)
        {
            var capturedRegistration = registration;
            server.OnCompletionControl((source, parts) =>
            {
                // Replacing a handler keeps one stable native trampoline and
                // publishes only the latest managed target.
                Interlocked.Exchange(ref invokedRegistration,
                    capturedRegistration);
                // Ownership survives the native callback return. The receiver
                // can hand the messages to its own execution context and close
                // them exactly once after processing.
                delivered.TrySetResult((source, parts));
            });
        }
        poller.Add(server, PollEventFlags.PollCompletion, 1);
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(50);

        using Message application = Message.From("application-unread");
        Assert.True(client.Send(serverRid).Message(application).Submit());

        using Message command = Message.From("relocation-ready");
        using Message generation = Message.From("generation-9");
        Assert.True(client.TrySendCompletionControl(
            serverRid, [command, generation]));
        Assert.Equal("relocation-ready", command.GetString());
        Assert.Equal("generation-9", generation.GetString());

        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(2)));
        var control = await delivered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(15, Volatile.Read(ref invokedRegistration));
        Assert.Equal(clientRid, control.Source);
        Assert.Collection(control.Parts,
            part => Assert.Equal("relocation-ready", part.GetString()),
            part => Assert.Equal("generation-9", part.GetString()));
        Zlink.MultipartClose(control.Parts);
        foreach (Message part in control.Parts)
            Assert.Throws<ObjectDisposedException>(() => _ = part.Size);

        using Received received = RecvWithRetry(server);
        Assert.Equal("application-unread", received.Parts[0].GetString());
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

        using Message request = Message.From("hello");
        Task<IReadOnlyList<Message>> replyTask = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using Received inbound = Received.Create();
        Assert.True(router.Recv(inbound));
        RoutingId sourceRid = inbound.RoutingId
            ?? throw new InvalidOperationException("missing routing id");
        ulong requestSeq = inbound.RequestSeq
            ?? throw new InvalidOperationException("missing request sequence");
        using Message admitted = Message.From("admitted");
        router.Reply(sourceRid, requestSeq)
            .Message(admitted)
            .Submit();
        IReadOnlyList<Message> reply = await replyTask;
        foreach (Message part in reply)
            part.Dispose();

        using Message update = Message.From("unsolicited");
        Assert.True(router.Send(sourceRid).Message(update).Submit());
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
        ulong requestSeq = inbound.RequestSeq
            ?? throw new InvalidOperationException("missing request sequence");
        foreach (Message part in inbound.Parts) part.Dispose();

        using Message admitted = Message.From("admitted");
        router.Reply(sourceRid, requestSeq)
            .Message(admitted)
            .Submit();
        IReadOnlyList<Message> reply = await replyTask;
        foreach (Message part in reply)
            part.Dispose();

        using Message update = Message.From("unsolicited");
        Assert.True(router.Send(sourceRid).Message(update).Submit());

        using Received delivered = RecvWithRetry(dealer);
        Assert.Equal("unsolicited", delivered.Parts[0].GetString());
        dealer.Disconnect(endpoint);
    }

    [Fact]
    public async Task dealer_received_reply_routes_same_sequence_to_source_peer()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateDealerSocket();
        using var clientA = ctx.CreateDealerSocket();
        using var clientB = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "dealer-directed-reply");
        server.Bind(endpoint);
        clientA.Connect(endpoint);
        clientB.Connect(endpoint);
        Thread.Sleep(50);

        using Message requestA = Message.From("from-a");
        using Message requestB = Message.From("from-b");
        Task<IReadOnlyList<Message>> requestATask = clientA.Request()
            .Message(requestA)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        Task<IReadOnlyList<Message>> requestBTask = clientB.Request()
            .Message(requestB)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();

        Received? receivedA = null;
        Received? receivedB = null;
        for (int i = 0; i < 2; i++)
        {
            Received received = RecvWithRetry(server);
            Assert.Equal(ReceivedMessageType.Request, received.MessageType);
            Assert.True(received.RequestSeq.HasValue);
            Assert.NotEqual(0UL, received.RequestSeq.Value);
            string payload = received.Parts[0].GetString();
            if (payload == "from-a")
                receivedA = received;
            else if (payload == "from-b")
                receivedB = received;
            else
            {
                received.Dispose();
                throw new InvalidOperationException(
                    $"Unexpected payload '{payload}'.");
            }
        }

        Assert.NotNull(receivedA);
        Assert.NotNull(receivedB);
        Assert.NotEqual(receivedA!.RequestSeq, receivedB!.RequestSeq);

        using Message replyB = Message.From("reply-b");
        using Message replyA = Message.From("reply-a");
        receivedB.Reply().Message(replyB).Submit();
        receivedA.Reply().Message(replyA).Submit();
        receivedA.Dispose();
        receivedB.Dispose();

        IReadOnlyList<Message> clientAReply = await requestATask;
        Assert.Equal("reply-a", clientAReply[0].GetString());
        Zlink.MultipartClose(clientAReply);

        IReadOnlyList<Message> clientBReply = await requestBTask;
        Assert.Equal("reply-b", clientBReply[0].GetString());
        Zlink.MultipartClose(clientBReply);
    }

    [Fact]
    public async Task dealer_received_reply_routes_when_request_uses_dontwait_callback()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateDealerSocket();
        using var client = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "dealer-dontwait-directed-reply");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(50);

        using Message request = Message.From("from-client");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        bool accepted = client.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            });

        Assert.True(accepted);

        using Received received = RecvWithRetry(server);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);
        Assert.Equal("from-client", received.Parts[0].GetString());

        using Message reply = Message.From("reply");
        received.Reply().Message(reply).Submit();

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("reply", clientReply[0].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public async Task dealer_received_reply_routes_over_tcp_when_request_uses_dontwait_callback()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateDealerSocket();
        using var client = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "dealer-dontwait-tcp-directed-reply");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("from-client");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        bool accepted = client.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            });

        Assert.True(accepted);

        using Received received = RecvWithRetry(server);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);
        Assert.Equal("from-client", received.Parts[0].GetString());

        using Message reply = Message.From("reply");
        received.Reply().Message(reply).Submit();

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("reply", clientReply[0].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public async Task dealer_received_multipart_reply_routes_over_tcp_when_request_uses_dontwait_callback()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateDealerSocket();
        using var client = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "dealer-dontwait-tcp-multipart-reply");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(100);

        using Message requestHeader = Message.From("request-header");
        using Message requestBody = Message.From("request-body");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        bool accepted = client.Request()
            .Messages([requestHeader, requestBody])
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            });

        Assert.True(accepted);

        using Received received = RecvWithRetry(server);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);
        Assert.Equal("request-header", received.Parts[0].GetString());
        Assert.Equal("request-body", received.Parts[1].GetString());

        using Message replyHeader = Message.From("reply-header");
        using Message replyBody = Message.From("reply-body");
        received.Reply().Messages([replyHeader, replyBody]).Submit();

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("reply-header", clientReply[0].GetString());
        Assert.Equal("reply-body", clientReply[1].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public async Task dealer_received_large_first_part_multipart_reply_routes_over_tcp()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateDealerSocket();
        using var client = ctx.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "dealer-large-first-part-reply");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(100);

        using Message requestHeader = Message.From("request-header");
        using Message requestBody = Message.From("request-body");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        bool accepted = client.Request()
            .Messages([requestHeader, requestBody])
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            });

        Assert.True(accepted);

        using Received received = RecvWithRetry(server);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);

        var replyHeaderText =
            """{"kind":2,"channelName":"profile.mesh","messageName":"ProfileRequest","contentType":"application/json","correlationId":"reply","deadline":null,"topic":null,"errorCode":null,"errorMessage":null,"source":null}""";
        using Message replyHeader = Message.From(replyHeaderText);
        using Message replyBody = Message.From("""{"value":"reply","providerRid":"api-a"}""");
        received.Reply().Messages([replyHeader, replyBody]).Submit();

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(replyHeaderText, clientReply[0].GetString());
        Assert.Equal("""{"value":"reply","providerRid":"api-a"}""", clientReply[1].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public async Task dealer_received_reply_routes_from_one_of_two_bound_tcp_peers_when_request_uses_dontwait_callback()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var serverAContext = Zlink.CreateContext();
        using var serverBContext = Zlink.CreateContext();
        using var clientContext = Zlink.CreateContext();
        using var serverA = serverAContext.CreateDealerSocket();
        using var serverB = serverBContext.CreateDealerSocket();
        using var client = clientContext.CreateDealerSocket();

        string endpointA = CoreTestSupport.NewEndpoint("tcp",
            "dealer-dontwait-two-peers-a");
        string endpointB = CoreTestSupport.NewEndpoint("tcp",
            "dealer-dontwait-two-peers-b");
        serverA.Bind(endpointA);
        serverB.Bind(endpointB);
        client.Connect(endpointA);
        client.Connect(endpointB);
        Thread.Sleep(100);

        using Message requestHeader = Message.From("request-header");
        using Message requestBody = Message.From("request-body");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        bool accepted = client.Request()
            .Messages([requestHeader, requestBody])
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            });

        Assert.True(accepted);

        Received received = RecvWithRetry(serverA, serverB);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);
        Assert.Equal("request-header", received.Parts[0].GetString());
        Assert.Equal("request-body", received.Parts[1].GetString());

        using Message replyHeader = Message.From("reply-header");
        using Message replyBody = Message.From("reply-body");
        received.Reply().Messages([replyHeader, replyBody]).Submit();
        received.Dispose();

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("reply-header", clientReply[0].GetString());
        Assert.Equal("reply-body", clientReply[1].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public async Task dealer_received_reply_routes_with_framework_dealer_mesh_options()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var serverA = ctx.CreateDealerSocket();
        using var serverB = ctx.CreateDealerSocket();
        using var client = ctx.CreateDealerSocket();

        serverA.Options.PeerWeight = 100;
        serverB.Options.PeerWeight = 100;
        client.Options.PeerWeight = 100;
        serverA.OnSendReady(() => { });
        serverB.OnSendReady(() => { });
        client.OnSendReady(() => { });

        string endpointA = CoreTestSupport.NewEndpoint("tcp",
            "dealer-framework-options-a");
        string endpointB = CoreTestSupport.NewEndpoint("tcp",
            "dealer-framework-options-b");
        serverA.Bind(endpointA);
        serverB.Bind(endpointB);
        client.Connect(endpointA);
        client.Connect(endpointB);
        Thread.Sleep(100);

        using Message requestHeader = Message.From("request-header");
        using Message requestBody = Message.From("request-body");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        bool accepted = client.Request()
            .Messages([requestHeader, requestBody])
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            });

        Assert.True(accepted);

        using Received received = RecvWithRetry(serverA, serverB);
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        Assert.True(received.RequestSeq.HasValue);
        Assert.Equal("request-header", received.Parts[0].GetString());
        Assert.Equal("request-body", received.Parts[1].GetString());

        using Message replyHeader = Message.From("reply-header");
        using Message replyBody = Message.From("reply-body");
        received.Reply().Messages([replyHeader, replyBody]).Submit();

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("reply-header", clientReply[0].GetString());
        Assert.Equal("reply-body", clientReply[1].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public async Task dealer_received_reply_routes_when_submitted_from_another_thread()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var serverContext = Zlink.CreateContext();
        using var clientContext = Zlink.CreateContext();
        using var server = serverContext.CreateDealerSocket();
        using var client = clientContext.CreateDealerSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "dealer-reply-from-another-thread");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("request");
        var completion = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(client.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Flags(SendFlags.DontWait)
            .Submit((result, parts) =>
            {
                if (result != RequestResult.Ok)
                {
                    completion.TrySetException(
                        new InvalidOperationException($"request failed: {result}"));
                    return;
                }

                completion.TrySetResult(parts);
            }));

        Received received = RecvWithRetry(server);
        await Task.Run(() =>
        {
            using Message reply = Message.From("reply");
            received.Reply().Message(reply).Submit();
            received.Dispose();
        });

        IReadOnlyList<Message> clientReply = await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("reply", clientReply[0].GetString());
        Zlink.MultipartClose(clientReply);
    }

    [Fact]
    public void request_router_preserves_data_receive_surface()
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
        Thread.Sleep(50);

        using Message payload = Message.From("plain-data");
        dealerSocket.Send().Message(payload).Submit();

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
    public async Task request_callback_transfers_reply_message_ownership_to_application()
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
        Thread.Sleep(50);

        using var handled = new ManualResetEventSlim(false);
        using var callbackReceived = new ManualResetEventSlim(false);
        Message? owned = null;
        RequestResult observedResult = RequestResult.ProtocolError;

        Task serverTask = Task.Run(() =>
        {
            var received = Received.Create();
            routerSocket.Recv(received);
            try
            {
                using Message reply = Message.From("pong-owned");
                routerSocket.Reply(
                    received.RoutingId ?? throw new InvalidOperationException(
                        "missing routing id"), received.RequestSeq ?? 0UL)
                    .Message(reply).Submit();
                handled.Set();
            }
            finally
            {
                foreach (Message part in received.Parts) part.Dispose();
            }
        });

        using Message request = Message.From("ping-owned");
        dealerSocket.Request().Message(request).Submit((result, reply) =>
        {
            observedResult = result;
            Assert.Single(reply);
            owned = reply[0];
            callbackReceived.Set();
        });

        Assert.True(callbackReceived.Wait(10000));
        Assert.True(handled.Wait(10000));
        Assert.Equal(RequestResult.Ok, observedResult);
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
