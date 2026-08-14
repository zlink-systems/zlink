using System.Text;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_socket_concurrency
{
    [Fact]
    public async Task dealer_and_router_allow_concurrent_public_sends()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "concurrent-public-sends");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(50);

        const int producerCount = 4;
        const int messagesPerProducer = 32;
        const int messageCount = producerCount * messagesPerProducer;

        var dealerStart = new Barrier(producerCount);
        Task[] dealerSends = Enumerable.Range(0, producerCount)
            .Select(producer => Task.Run(async () =>
            {
                dealerStart.SignalAndWait();
                for (var messageIndex = 0;
                     messageIndex < messagesPerProducer;
                     messageIndex++)
                {
                    using Message header = Message.From(
                        $"dealer-{producer}-{messageIndex}-header");
                    using Message body = Message.From(
                        $"dealer-{producer}-{messageIndex}-body");
                    await dealer.Send()
                        .Message(header)
                        .Message(body)
                        .Async();
                }
            }))
            .ToArray();
        await Task.WhenAll(dealerSends).WaitAsync(TimeSpan.FromSeconds(10));
        dealerStart.Dispose();

        using var received = Received.Create();
        RoutingId? dealerRoutingId = null;
        var receivedDealerMessages = 0;
        var dealerPayloads = new HashSet<string>(StringComparer.Ordinal);
        while (receivedDealerMessages < messageCount)
        {
            Assert.True(CoreTestSupport.WaitUntil(
                () => router.Recv(received, RecvFlags.DontWait), 2000));
            dealerRoutingId ??= received.RoutingId;
            Assert.Equal(2, received.Parts.Count);
            dealerPayloads.Add(string.Join("|", received.Parts.Select(
                part => Encoding.UTF8.GetString(part.AsReadOnlySpan()))));
            receivedDealerMessages++;
        }

        Assert.Equal(messageCount, dealerPayloads.Count);
        Assert.All(dealerPayloads, payload =>
        {
            string[] parts = payload.Split('|');
            Assert.Equal(2, parts.Length);
            Assert.EndsWith("-header", parts[0], StringComparison.Ordinal);
            Assert.Equal(
                parts[0][..^"-header".Length] + "-body",
                parts[1]);
        });
        RoutingId target = dealerRoutingId
            ?? throw new InvalidOperationException(
                "Concurrent dealer send did not expose a routing id.");

        var routerStart = new Barrier(producerCount);
        Task[] routerSends = Enumerable.Range(0, producerCount)
            .Select(producer => Task.Run(async () =>
            {
                routerStart.SignalAndWait();
                for (var messageIndex = 0;
                     messageIndex < messagesPerProducer;
                     messageIndex++)
                {
                    using Message header = Message.From(
                        $"router-{producer}-{messageIndex}-header");
                    using Message body = Message.From(
                        $"router-{producer}-{messageIndex}-body");
                    await router.Send(target)
                        .Message(header)
                        .Message(body)
                        .Async();
                }
            }))
            .ToArray();
        await Task.WhenAll(routerSends).WaitAsync(TimeSpan.FromSeconds(10));
        routerStart.Dispose();

        var receivedRouterMessages = 0;
        var routerPayloads = new HashSet<string>(StringComparer.Ordinal);
        while (receivedRouterMessages < messageCount)
        {
            Assert.True(CoreTestSupport.WaitUntil(
                () => dealer.Recv(received, RecvFlags.DontWait), 2000));
            Assert.Equal(2, received.Parts.Count);
            routerPayloads.Add(string.Join("|", received.Parts.Select(
                part => Encoding.UTF8.GetString(part.AsReadOnlySpan()))));
            receivedRouterMessages++;
        }

        Assert.Equal(messageCount, routerPayloads.Count);
        Assert.All(routerPayloads, payload =>
        {
            string[] parts = payload.Split('|');
            Assert.Equal(2, parts.Length);
            Assert.EndsWith("-header", parts[0], StringComparison.Ordinal);
            Assert.Equal(
                parts[0][..^"-header".Length] + "-body",
                parts[1]);
        });
    }

    [Fact]
    public async Task dealer_and_router_allow_concurrent_multipart_requests_and_replies()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "concurrent-public-request-reply");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(50);

        const int requestCount = 32;
        var start = new Barrier(requestCount + 1);
        Task<IReadOnlyList<Message>>[] requests = Enumerable.Range(0, requestCount)
            .Select(requestIndex => Task.Run(async () =>
            {
                start.SignalAndWait();
                using Message header = Message.From(
                    $"request-{requestIndex}-header");
                using Message body = Message.From(
                    $"request-{requestIndex}-body");
                IReadOnlyList<Message> reply = await dealer.Request()
                    .Messages([header, body])
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async();
                try
                {
                    Assert.Equal(2, reply.Count);
                    Assert.Equal($"reply-{requestIndex}-header",
                        reply[0].GetString());
                    Assert.Equal($"reply-{requestIndex}-body",
                        reply[1].GetString());
                }
                finally
                {
                    Zlink.MultipartClose(reply);
                }

                return reply;
            }))
            .ToArray();
        start.SignalAndWait();
        var replies = new List<Task>(requestCount);
        for (var requestIndex = 0; requestIndex < requestCount; requestIndex++)
        {
            using Received received = ReceiveWithRetry(router);
            RoutingId routingId = received.RoutingId
                ?? throw new InvalidOperationException("missing routing id");
            ulong requestSeq = received.RequestSeq
                ?? throw new InvalidOperationException("missing request sequence");
            string requestHeader = received.Parts[0].GetString();
            int requestNumber = int.Parse(
                requestHeader["request-".Length..^"-header".Length]);
            replies.Add(Task.Run(() =>
            {
                using Message header = Message.From(
                    $"reply-{requestNumber}-header");
                using Message body = Message.From(
                    $"reply-{requestNumber}-body");
                router.Reply(routingId, requestSeq)
                    .Messages([header, body])
                    .Submit();
            }));
        }

        await Task.WhenAll(replies).WaitAsync(TimeSpan.FromSeconds(10));
        await Task.WhenAll(requests).WaitAsync(TimeSpan.FromSeconds(10));
        start.Dispose();
    }

    [Fact]
    public async Task dealer_allows_concurrent_send_and_request_submission()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "concurrent-public-send-request");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(50);

        const int producerCount = 4;
        const int operationsPerProducer = 8;
        const int operationCount = producerCount * operationsPerProducer;
        var start = new Barrier(producerCount * 2 + 1);
        var requests = new List<Task>(operationCount);
        var sends = new List<Task>(operationCount);

        for (var producer = 0; producer < producerCount; producer++)
        {
            var producerIndex = producer;
            sends.Add(Task.Run(async () =>
            {
                start.SignalAndWait();
                for (var operation = 0; operation < operationsPerProducer; operation++)
                {
                    using Message header = Message.From(
                        $"send-{producerIndex}-{operation}-header");
                    using Message body = Message.From(
                        $"send-{producerIndex}-{operation}-body");
                    await dealer.Send()
                        .Messages([header, body])
                        .Async();
                }
            }));

            requests.Add(Task.Run(async () =>
            {
                start.SignalAndWait();
                for (var operation = 0; operation < operationsPerProducer; operation++)
                {
                    using Message header = Message.From(
                        $"request-{producerIndex}-{operation}-header");
                    using Message body = Message.From(
                        $"request-{producerIndex}-{operation}-body");
                    IReadOnlyList<Message> reply = await dealer.Request()
                        .Messages([header, body])
                        .Timeout(TimeSpan.FromSeconds(5))
                        .Async();
                    try
                    {
                        Assert.Equal(2, reply.Count);
                        Assert.Equal($"reply-{producerIndex}-{operation}-header",
                            reply[0].GetString());
                        Assert.Equal($"reply-{producerIndex}-{operation}-body",
                            reply[1].GetString());
                    }
                    finally
                    {
                        Zlink.MultipartClose(reply);
                    }
                }
            }));
        }
        start.SignalAndWait();

        var rawPayloads = new HashSet<string>(StringComparer.Ordinal);
        var replyTasks = new List<Task>(operationCount);
        for (var receivedCount = 0; receivedCount < operationCount * 2; receivedCount++)
        {
            using Received received = ReceiveWithRetry(router);
            if (received.MessageType != ReceivedMessageType.Request)
            {
                rawPayloads.Add(string.Join("|", received.Parts.Select(
                    part => part.GetString())));
                continue;
            }

            RoutingId routingId = received.RoutingId
                ?? throw new InvalidOperationException("missing routing id");
            ulong requestSeq = received.RequestSeq
                ?? throw new InvalidOperationException("missing request sequence");
            string requestHeader = received.Parts[0].GetString();
            string requestPrefix = requestHeader["request-".Length..^"-header".Length];
            replyTasks.Add(Task.Run(() =>
            {
                using Message header = Message.From($"reply-{requestPrefix}-header");
                using Message body = Message.From($"reply-{requestPrefix}-body");
                router.Reply(routingId, requestSeq)
                    .Messages([header, body])
                    .Submit();
            }));
        }

        await Task.WhenAll(replyTasks).WaitAsync(TimeSpan.FromSeconds(10));
        await Task.WhenAll(sends).WaitAsync(TimeSpan.FromSeconds(10));
        await Task.WhenAll(requests).WaitAsync(TimeSpan.FromSeconds(10));
        start.Dispose();
        Assert.Equal(operationCount, rawPayloads.Count);
        Assert.All(rawPayloads, payload =>
        {
            string[] parts = payload.Split('|');
            Assert.Equal(2, parts.Length);
            Assert.EndsWith("-header", parts[0], StringComparison.Ordinal);
            Assert.Equal(parts[0][..^"-header".Length] + "-body", parts[1]);
        });
    }

    private static Received ReceiveWithRetry(IRouterSocket socket)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
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
}
