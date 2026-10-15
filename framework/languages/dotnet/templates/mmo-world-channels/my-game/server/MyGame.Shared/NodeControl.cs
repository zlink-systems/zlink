using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Configuration;

namespace MyGame.Shared;

public static class NodeControl
{
    public static async Task RunAsync(
        IHost host,
        NodeSettings settings,
        Func<CancellationToken, Task>? initialize = null
    )
    {
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        if (initialize is not null)
            await initialize(CancellationToken.None);
        var mesh = runtime.GetListenerStatus(ZLinkListenerKind.RouteMesh, settings.MeshName);
        var stream = settings.StreamEndpoint is null
            ? null
            : runtime.GetListenerStatus(ZLinkListenerKind.Stream, GameNames.SessionStream).Endpoint;
        await CommandsAsync(host, new(settings.Id, mesh.Endpoint, stream), runtime);
    }

    public static async Task RunAccountsAsync(IHost host, Func<NodeReady> readiness)
    {
        await host.StartAsync();
        await CommandsAsync(
            host,
            readiness(),
            host.Services.GetRequiredService<IZLinkFrameworkRuntime>()
        );
    }

    private static async Task CommandsAsync(
        IHost host,
        NodeReady ready,
        IZLinkFrameworkRuntime runtime
    )
    {
        Console.WriteLine(
            NodeProtocol.ReadyPrefix
                + JsonSerializer.Serialize(
                    ready,
                    new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase }
                )
        );
        while (await Console.In.ReadLineAsync() is { } command)
        {
            switch (command.Trim())
            {
                case NodeProtocol.Drain:
                    Console.WriteLine(NodeProtocol.RelocationRequested);
                    var relocated = await runtime.RelocateAsync(
                        new ZLinkFrameworkRelocationOptions
                        {
                            Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                        }
                    );
                    if (relocated.Outcome != ZLinkFrameworkRelocationOutcome.Relocated)
                        throw new InvalidOperationException(
                            $"Host relocation blocked: {relocated.Reason}."
                        );
                    Console.WriteLine(NodeProtocol.Relocated);
                    await foreach (var observed in runtime.ObserveAsync())
                    {
                        if (!observed.Status.SafeToShutdown)
                            continue;
                        Console.WriteLine(NodeProtocol.SafeToShutdown);
                        break;
                    }
                    break;
                case NodeProtocol.Shutdown:
                    var stopped = await runtime.ShutdownAsync();
                    if (stopped.Outcome != ZLinkFrameworkTerminationOutcome.Stopped)
                        throw new InvalidOperationException(
                            $"Host shutdown failed: {stopped.Reason}."
                        );
                    await host.StopAsync();
                    Console.WriteLine(NodeProtocol.Stopped);
                    return;
                default:
                    throw new InvalidDataException($"Unsupported command '{command}'.");
            }
        }
        await host.StopAsync();
    }
}
