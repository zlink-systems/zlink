using System.Threading;
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
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        int port = SampleSupport.ExtractPort(endpoint);
        using var monitor = stream.MonitorOpen(SocketEvent.Accepted);
        stream.Bind(endpoint);

        using var signal = new ManualResetEventSlim(false);
        string? callbackPayload = null;
        StreamPacketHandler handler = (routingId, header, body) =>
        {
            using (header)
            using (body)
            {
                callbackPayload = body.GetString();
            }
            signal.Set();
        };
        stream.OnPacket(handler);

        using var client = SampleSupport.ConnectRawClient(port);
        SampleSupport.WaitMonitorEvent(monitor, 5000, SocketEvent.Accepted);
        SampleSupport.SendStreamPacket(client.GetStream(), "hello-stream"u8);
        SampleSupport.WaitOrThrow(() => signal.IsSet, 2000,
            "stream packet callback timeout");
        Console.WriteLine(
            $"[stream/packet-callback] send: \"hello-stream\" -> recv: \"{callbackPayload}\"");
        // --8<-- [end:doc]
    }
}
