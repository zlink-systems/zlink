using System.Net.Sockets;
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

        using var client = SampleSupport.ConnectRawClient(port);
        SampleSupport.WaitMonitorEvent(monitor, 5000, SocketEvent.Accepted);
        NetworkStream network = client.GetStream();
        byte[] request = "hello-stream"u8.ToArray();
        SampleSupport.SendAll(network, request);

        using var received = Received.Create();
        if (!stream.Recv(received))
            throw new InvalidOperationException("recv failed");
        if (received.RoutingId == null)
            throw new InvalidOperationException("missing routing id");
        string payload = received.Parts[0].GetString();
        SampleSupport.EnsureEqual("hello-stream", payload, "payload");

        using var reply = Message.From("hello-stream");
        received.Send().Message(reply).Submit();
        string echoed = System.Text.Encoding.UTF8.GetString(
            SampleSupport.ReceiveExact(network, "hello-stream".Length));
        Console.WriteLine(
            $"[stream/recv] send: \"hello-stream\" -> recv: \"{echoed}\"");
        // --8<-- [end:doc]
    }
}
