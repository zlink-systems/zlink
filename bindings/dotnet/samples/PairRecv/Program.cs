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
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        using var senderMonitor = sender.MonitorOpen(SocketEvent.ConnectionReady);
        using var receiverMonitor = receiver.MonitorOpen(SocketEvent.ConnectionReady);
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        SampleSupport.WaitConnected(senderMonitor, receiverMonitor);

        const string payload = "hello-pair";
        using (Message message = Message.From(payload))
            sender.Send().Message(message).Submit();
        string receivedPayload = SampleSupport.ReceiveUtf8(receiver, 2000);
        Console.WriteLine(
            $"[pair/recv] send: \"hello-pair\" -> recv: \"{receivedPayload}\"");
        // --8<-- [end:doc]
    }
}
