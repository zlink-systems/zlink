using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Configuration;

internal sealed class ReadySignalHostedService(
    IHostApplicationLifetime applicationLifetime,
    string? readyFilePath,
    string? stopFilePath,
    TestHostOptions options,
    IServiceProvider services
) : IHostedService
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
        var listener = options.Mode switch
        {
            "channel-server" => (ZLinkListenerKind.ClientServer, options.ChannelName),
            "channel-publisher" => (ZLinkListenerKind.Fanout, options.ChannelName),
            "route-server"
            or "entry-spot-source"
            or "entry-spot-target"
            or "user-spot-source"
            or "user-spot-target" => (
                ZLinkListenerKind.RouteMesh,
                options.MeshName ?? options.ChannelName
            ),
            "stream-raw" => (ZLinkListenerKind.Stream, "stream.raw"),
            _ => (ZLinkListenerKind.RouteMesh, null),
        };
        var endpoint = listener.Item2 is null
            ? null
            : services
                .GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(listener.Item1, listener.Item2)
                .Endpoint;
        var payload = JsonSerializer.Serialize(
            new
            {
                app = "Zlink.Framework.TestHost",
                mode = options.Mode,
                pid = Environment.ProcessId,
                endpoint,
            }
        );

        Console.WriteLine($"READY:{payload}");

        if (!string.IsNullOrWhiteSpace(readyFilePath))
            File.WriteAllText(readyFilePath, payload);
    }

    private static async Task ListenForStopSignalAsync(
        IHostApplicationLifetime lifetime,
        CancellationToken cancellationToken
    )
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
        CancellationToken cancellationToken
    )
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
