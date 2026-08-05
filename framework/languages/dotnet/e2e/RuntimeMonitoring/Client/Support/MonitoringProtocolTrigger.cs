using System.Net.Sockets;

namespace RuntimeMonitoring.Client.Support;

internal static class MonitoringProtocolTrigger
{
    public static async Task SendInvalidHandshakeAsync(string endpoint)
    {
        var uri = new Uri(endpoint);
        using var client = new TcpClient();
        await client.ConnectAsync(uri.Host, uri.Port);
        var stream = client.GetStream();
        var invalidGreeting = new byte[]
        {
            0x00, 0x01, 0x02, 0x00,
            0x00, 0x00, 0x00, 0x03,
            0x01, 0x00, 0x00
        };
        await stream.WriteAsync(invalidGreeting);
        await stream.FlushAsync();
        client.Client.Shutdown(SocketShutdown.Send);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        var response = new byte[1];
        try
        {
            _ = await stream.ReadAsync(response, timeout.Token);
        }
        catch (IOException)
        {
            // A protocol rejection may reset the connection instead of returning EOF.
        }
    }
}
