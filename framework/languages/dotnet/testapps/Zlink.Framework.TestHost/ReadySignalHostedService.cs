using System.Text.Json;
using Microsoft.Extensions.Hosting;

internal sealed class ReadySignalHostedService(
    IHostApplicationLifetime applicationLifetime,
    string? readyFilePath,
    string? stopFilePath,
    string mode) : IHostedService
{
    public Task StartAsync(CancellationToken cancellationToken)
    {
        applicationLifetime.ApplicationStarted.Register(WriteReadyMarker);

        if (!string.IsNullOrWhiteSpace(stopFilePath))
            _ = WatchStopFileAsync(applicationLifetime, stopFilePath, cancellationToken);
        else
            _ = ListenForStopSignalAsync(applicationLifetime, cancellationToken);

        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        return Task.CompletedTask;
    }

    private void WriteReadyMarker()
    {
        var payload = JsonSerializer.Serialize(new
        {
            app = "Zlink.Framework.TestHost",
            mode,
            pid = Environment.ProcessId
        });

        Console.WriteLine($"READY:{payload}");

        if (!string.IsNullOrWhiteSpace(readyFilePath)) File.WriteAllText(readyFilePath, payload);
    }

    private static async Task ListenForStopSignalAsync(
        IHostApplicationLifetime lifetime,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var line = await Console.In.ReadLineAsync(cancellationToken);

            if (line is null || string.Equals(line, "STOP", StringComparison.Ordinal))
            {
                lifetime.StopApplication();
                return;
            }
        }
    }

    private static async Task WatchStopFileAsync(
        IHostApplicationLifetime lifetime,
        string stopFilePath,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            if (File.Exists(stopFilePath))
            {
                lifetime.StopApplication();
                return;
            }

            await Task.Delay(100, cancellationToken);
        }
    }
}