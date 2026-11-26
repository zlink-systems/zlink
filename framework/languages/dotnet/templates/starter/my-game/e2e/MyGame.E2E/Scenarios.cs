using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class Scenarios
{
    private const string Connect = "connect";
    private static readonly TimeSpan ClientTimeout = TimeSpan.FromSeconds(10);
    public static readonly string[] All = [Connect];

    public static string[] Select(string? option)
    {
        var selected = option?.Split(',', StringSplitOptions.RemoveEmptyEntries) ?? All;
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(id => !All.Contains(id))
        )
            throw new ArgumentException(
                "Unknown or duplicate scenario. Available: " + string.Join(',', All)
            );
        return All.Where(selected.Contains).ToArray();
    }

    public static async Task RunAsync(
        WorldRun run,
        string[] selected,
        RunLog log,
        CancellationToken ct
    )
    {
        foreach (var scenario in selected)
        {
            log.Start(scenario);
            await ConnectAsync(run, ct);
            log.Pass(scenario);
        }
    }

    // Add application scenarios here and extend All and zlink.structure.json together.
    private static async Task ConnectAsync(WorldRun run, CancellationToken ct)
    {
        var client = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(run.Endpoint),
                ConnectTimeout = ClientTimeout,
                RequestTimeout = ClientTimeout,
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            }
        );
        try
        {
            await client.Connect.Async(ct);
        }
        finally
        {
            await run.Outcome.CleanupAsync(client.DisposeAsync);
        }
        if (run.IsAttached)
            return;
        await run.Session.CommandAsync(NodeProtocol.Drain);
        await run.Session.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
        await run.Session.WaitLineAsync(NodeProtocol.Relocated, ct);
        await run.Session.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
        await run.Session.StopAsync();
    }
}
