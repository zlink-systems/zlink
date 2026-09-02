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
        using var stream = ctx.CreateStreamSocket();
        stream.Options.Linger = TimeSpan.Zero;
        stream.Options.ReceiveMode = StreamReceiveMode.Packet;
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        int port = SampleSupport.ExtractPort(endpoint);
        using var monitor = stream.MonitorOpen(SocketEvent.Accepted);
        stream.Bind(endpoint);

        using var client = SampleSupport.ConnectRawClient(port);
        SampleSupport.WaitMonitorEvent(monitor, 5000, SocketEvent.Accepted);
        SampleSupport.SendStreamPacket(client.GetStream(), "hello-stream"u8);
        using var packet = StreamPacket.Create();
        if (!stream.RecvPacket(packet))
            throw new InvalidOperationException("packet receive failed");
        string callbackPayload = packet.Body?.GetString()
            ?? throw new InvalidOperationException("missing packet body");
        Console.WriteLine(
            $"[stream/packet-pull] send: \"hello-stream\" -> recv: \"{callbackPayload}\"");
        // --8<-- [end:doc]
    }
}
