using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Support;

internal static class ProcessSupport
{
    public static int PickPort()
    {
        using var socket = new TcpListener(IPAddress.Loopback, 0);
        socket.Start();
        var port = ((IPEndPoint)socket.LocalEndpoint).Port;
        socket.Stop();
        return port;
    }

    public static async Task CopyToFileAsync(StreamReader reader, string path)
    {
        await using var stream = File.Open(path, FileMode.Create, FileAccess.Write, FileShare.Read);
        await using var writer = new StreamWriter(stream);
        while (await reader.ReadLineAsync() is { } line)
        {
            await writer.WriteLineAsync(line);
            await writer.FlushAsync();
        }
    }

    public static async Task WaitHealthAsync(ZLinkHttpClient http, Process process, string failureContext)
    {
        for (var i = 0; i < 120; i++)
        {
            if (process.HasExited)
                throw new InvalidOperationException($"{failureContext} server exited early: {process.ExitCode}.");

            try
            {
                if ((await http.Get("/health").AsyncRaw()).Status == 200) return;
            }
            catch (Exception ex) when (
                ex is HttpRequestException || ex.InnerException is HttpRequestException)
            {
            }

            await Task.Delay(250);
        }

        throw new TimeoutException($"Timed out waiting for {failureContext} server.");
    }
}
