using SampleCommon;
using Systems.Zlink;

internal static class Program
{
    private static void Main(string[] args)
    {
        // --8<-- [start:doc]
        if (!SampleSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var publisher = ctx.CreatePubSocket();
        using var subscriber = ctx.CreateSubSocket();
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        using var publisherMonitor = publisher.MonitorOpen(SocketEvent.ConnectionReady);
        using var subscriberMonitor = subscriber.MonitorOpen(SocketEvent.ConnectionReady);
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        SampleSupport.WaitConnected(publisherMonitor, subscriberMonitor);
        subscriber.SetSubscription("prices");

        using (Message message = Message.From("101.25"))
            publisher.Publish("prices").Message(message).Submit();
        string payload = SampleSupport.SubscribeUtf8(subscriber, out string topic, 2000);
        Console.WriteLine(
            $"[pubsub/recv] publish: \"prices/101.25\" -> subscribe: \"{topic}/{payload}\"");
        // --8<-- [end:doc]
    }
}
