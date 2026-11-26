using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Configuration;

namespace MyGame.Shared;

public static class NodeControl
{
    private static async Task WaitForServingAsync(
        IZLinkFrameworkRuntime runtime,
        CancellationToken cancellationToken
    )
    {
        await foreach (var observed in runtime.ObserveAsync(cancellationToken))
        {
            if (observed.Loss.DiscardedTerminalCount != 0)
                throw new InvalidOperationException("Startup observation lost.");
            if (observed.Status.State == ZLinkFrameworkRuntimeState.Serving)
                return;
            if (observed.Status.State != ZLinkFrameworkRuntimeState.Preparing)
                throw new InvalidOperationException(
                    $"Runtime startup failed: {observed.Status.State}"
                );
        }
        throw new InvalidOperationException("Runtime observation ended before Serving.");
    }

    public static async Task RunAsync(
        IHost host,
        NodeSettings settings,
        Func<CancellationToken, Task>? initialize = null
    )
    {
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        await WaitForServingAsync(runtime, CancellationToken.None);
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
        var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        await WaitForServingAsync(runtime, CancellationToken.None);
        await CommandsAsync(host, readiness(), runtime);
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
                {
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
                    using var safeDeadline = new CancellationTokenSource(NodeProtocol.SafeDeadline);
                    await foreach (var observed in runtime.ObserveAsync(safeDeadline.Token))
                    {
                        if (!observed.Status.SafeToShutdown)
                            continue;
                        Console.WriteLine(NodeProtocol.SafeToShutdown);
                        break;
                    }
                    break;
                }
                case NodeProtocol.CreateUpdateZone:
                    var settings = host.Services.GetRequiredService<NodeSettings>();
                    await GameHosting.CreateZoneAsync(
                        host.Services.GetRequiredService<Zlink.Framework.Contracts.Spots.IZLinkSpotManager>(),
                        settings.MeshName,
                        GameNames.ZoneId(GameNames.UpdateChannel),
                        CancellationToken.None
                    );
                    Console.WriteLine(NodeProtocol.UpdateZoneCreated);
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
