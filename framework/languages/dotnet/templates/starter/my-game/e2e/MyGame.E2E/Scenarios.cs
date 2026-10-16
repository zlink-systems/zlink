using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["connect"];

    public static string[] Select(string? option)
    {
        var selected = option?.Split(',', StringSplitOptions.RemoveEmptyEntries) ?? All;
        if (selected.Length == 0 || selected.Distinct().Count() != selected.Length || selected.Any(id => !All.Contains(id)))
            throw new ArgumentException("Unknown or duplicate scenario. Available: " + string.Join(',', All));
        return All.Where(selected.Contains).ToArray();
    }

    public static async Task RunAsync(WorldRun run, string[] selected, CancellationToken ct)
    {
        var passed = 0;
        try
        {
            foreach (var id in selected)
            {
                try
                {
                    switch (id)
                    {
                        case "connect":
                            await ConnectAsync(run, ct);
                            break;
                    }
                    Console.WriteLine($"✓ {id}");
                    passed++;
                }
                catch
                {
                    Console.WriteLine($"✗ {id}");
                    throw;
                }
            }
        }
        finally
        {
            Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
        }
    }

    // Add application scenarios here and extend All and zlink.structure.json together.
    private static async Task ConnectAsync(WorldRun run, CancellationToken ct)
    {
        await using (var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(run.Endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(10),
            RequestTimeout = TimeSpan.FromSeconds(10),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
        }))
        {
            await client.Connect.Async(ct);
        }
        await run.Session.CommandAsync(NodeProtocol.Drain);
        await run.Session.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
        await run.Session.WaitLineAsync(NodeProtocol.Relocated, ct);
        await run.Session.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
        await run.Session.StopAsync();
    }
}
