using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using SampleCommon;
using Systems.Zlink;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        // --8<-- [start:doc]
        if (!SampleSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealerSocket = ctx.CreateDealerSocket();
        using var routerSocket = ctx.CreateRouterSocket();
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        using var dealerMonitor = dealerSocket.MonitorOpen(SocketEvent.ConnectionReady);
        using var routerMonitor = routerSocket.MonitorOpen(SocketEvent.ConnectionReady);
        dealerSocket.SetRoutingId(RoutingId.From(Encoding.UTF8.GetBytes("request-reply-client")));
        routerSocket.Bind(endpoint);
        dealerSocket.Connect(endpoint);
        SampleSupport.WaitConnected(routerMonitor, dealerMonitor);

        using var requestHandled = new ManualResetEventSlim(false);
        Task serverTask = Task.Run(() =>
        {
            try
            {
                using var received = Received.Create();
                if (!routerSocket.Recv(received))
                    throw new InvalidOperationException("recv failed");
                RoutingId routingId = received.RoutingId
                    ?? throw new InvalidOperationException("missing routing id");
                string requestPayload = received.Parts[0].GetString();
                SampleSupport.EnsureEqual("ping", requestPayload, "request");
                using var reply = Message.From("pong");
                routerSocket.Reply(routingId, received.RequestSeq ?? 0UL)
                    .Message(reply).Submit();
            }
            finally
            {
                requestHandled.Set();
            }
        });

        using var sent = Message.From("ping");
        IReadOnlyList<Message> replyReceived = await dealerSocket.Request()
            .Message(sent)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using Message replyPart = replyReceived[0];
        SampleSupport.EnsureEqual("pong", replyPart.GetString(), "reply");
        for (int i = 1; i < replyReceived.Count; i++)
            replyReceived[i].Dispose();
        SampleSupport.WaitOrThrow(() => requestHandled.IsSet, 2000, "request/reply async sample");
        await serverTask;
        Console.WriteLine("[dealer-router/request-reply/async] send: \"ping\" -> recv: \"pong\"");
        // --8<-- [end:doc]
    }
}
