using SampleCommon;
using Systems.Zlink;

internal static class Program
{
    private static void Main(string[] args)
    {
        if (!SampleSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        string endpoint = SampleSupport.NewEndpoint("tcp", "sample");
        using var serverMonitor = server.MonitorOpen(SocketEvent.ConnectionReady);
        using var clientMonitor = client.MonitorOpen(SocketEvent.ConnectionReady);
        if (serverMonitor.Recv(RecvFlags.DontWait) != null)
            throw new InvalidOperationException("monitor sample expected empty server Recv(true)");
        if (clientMonitor.Recv(RecvFlags.DontWait) != null)
            throw new InvalidOperationException("monitor sample expected empty client Recv(true)");
        server.Bind(endpoint);
        client.Connect(endpoint);

        MonitorEvent serverEvent = serverMonitor.Recv()
            ?? throw new TimeoutException("Timed out waiting for server monitor event.");
        MonitorEvent clientEvent = clientMonitor.Recv()
            ?? throw new TimeoutException("Timed out waiting for client monitor event.");
        if (serverEvent.Event != MonitorEventType.ConnectionReady
            || clientEvent.Event != MonitorEventType.ConnectionReady)
        {
            throw new InvalidOperationException("monitor sample expected ConnectionReady events");
        }
        if (serverMonitor.Recv(RecvFlags.DontWait) != null)
            throw new InvalidOperationException("monitor sample expected empty server Recv(true)");
        if (clientMonitor.Recv(RecvFlags.DontWait) != null)
            throw new InvalidOperationException("monitor sample expected empty client Recv(true)");

        Console.WriteLine("[monitor/recv] recv: \"connection-ready\" -> Recv(true): empty");
    }
}
