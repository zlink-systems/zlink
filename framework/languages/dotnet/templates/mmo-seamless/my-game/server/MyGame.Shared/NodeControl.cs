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
        Func<CancellationToken, Task>? bootstrap = null
    )
    {
        using var lifetime = new CancellationTokenSource();
        await host.StartAsync(lifetime.Token);
        var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        if (bootstrap is not null)
            await bootstrap(lifetime.Token);
        var mesh = runtime.GetListenerStatus(ZLinkListenerKind.RouteMesh, GameNames.WorldMesh);
        var stream = settings.StreamEndpoint is null
            ? null
            : runtime.GetListenerStatus(ZLinkListenerKind.Stream, GameNames.SessionStream).Endpoint;
        Console.WriteLine(
            NodeProtocol.ReadyPrefix
                + JsonSerializer.Serialize(new NodeReady(settings.Id, mesh.Endpoint, stream))
        );
        while (await Console.In.ReadLineAsync(lifetime.Token) is { } command)
        {
            switch (command.Trim())
            {
                case NodeProtocol.VacateZone:
                    var spots =
                        host.Services.GetRequiredService<Zlink.Framework.Contracts.Spots.IZLinkSpotManager>();
                    var spot =
                        await spots.FindAsync(
                            GameNames.ZoneId(NodeProtocol.ReplacementX),
                            lifetime.Token
                        ) ?? throw new InvalidOperationException("Idle Zone not found.");
                    if (!await spots.CloseAsync(spot, lifetime.Token))
                        throw new InvalidOperationException("Idle Zone close failed.");
                    Console.WriteLine(NodeProtocol.ZoneVacated);
                    break;
                case NodeProtocol.Drain:
                    Console.WriteLine(NodeProtocol.RelocationRequested);
                    var relocation = runtime
                        .RelocateAsync(
                            new ZLinkFrameworkRelocationOptions
                            {
                                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                            },
                            lifetime.Token
                        )
                        .AsTask();
                    var result = await relocation;
                    if (result.Outcome != ZLinkFrameworkRelocationOutcome.Relocated)
                        throw new InvalidOperationException(
                            $"Host relocation blocked: {result.Reason}."
                        );
                    Console.WriteLine(NodeProtocol.Relocated);
                    await foreach (var observed in runtime.ObserveAsync(lifetime.Token))
                    {
                        if (!observed.Status.SafeToShutdown)
                            continue;
                        Console.WriteLine(NodeProtocol.SafeToShutdown);
                        break;
                    }
                    break;
                case NodeProtocol.Shutdown:
                    var stopped = await runtime.ShutdownAsync(cancellationToken: lifetime.Token);
                    if (stopped.Outcome != ZLinkFrameworkTerminationOutcome.Stopped)
                        throw new InvalidOperationException(
                            $"Host shutdown failed: {stopped.Reason}."
                        );
                    await host.StopAsync(lifetime.Token);
                    return;
                default:
                    throw new InvalidDataException($"Unknown node command '{command}'.");
            }
        }
        await host.WaitForShutdownAsync(lifetime.Token);
    }
}
