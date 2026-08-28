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
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        using var dealerMonitor = dealer.MonitorOpen(SocketEvent.ConnectionReady);
        using var routerMonitor = router.MonitorOpen(SocketEvent.ConnectionReady);
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        SampleSupport.WaitConnected(routerMonitor, dealerMonitor);

        using (Message request = Message.From("ping"))
            await dealer.Send().Message(request).Async();
        using var received = Received.Create();
        if (!router.Recv(received))
            throw new InvalidOperationException("recv failed");
        string requestPayload = received.Parts[0].GetString();
        SampleSupport.EnsureEqual("ping", requestPayload, "request");

        using var reply = Message.From("pong");
        received.Send().Message(reply).Submit(SendFlags.None);
        string payload = SampleSupport.ReceiveUtf8(dealer, 2000);
        Console.WriteLine($"[dealer-router/recv] send: \"ping\" -> recv: \"{payload}\"");
        // --8<-- [end:doc]
    }
}
