using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class WorldRun(string root) : IAsyncDisposable
{
    private readonly List<ManagedProcess> nodes = [];
    private readonly string project = "my-game-e2e-" + Guid.NewGuid().ToString("N");
    private readonly string runDirectory = Path.Combine(
        root,
        ".logs",
        "run-" + Guid.NewGuid().ToString("N")
    );
    private bool composeStarted;
    private string redisEndpoint = "";
    public ManagedProcess ZoneA { get; private set; } = null!;
    public string Endpoint { get; private set; } = "";

    private string[] Compose(params string[] arguments) =>
        ["compose", "-f", Path.Combine(root, "docker-compose.yml"), "-p", project, .. arguments];

    public async Task StartAsync(CancellationToken ct)
    {
        Directory.CreateDirectory(runDirectory);
        Console.WriteLine($"Run logs: {runDirectory}");
        composeStarted = true;
        await Commands.RunAsync(
            "docker",
            Compose("up", "-d", "--wait", "--wait-timeout", "30"),
            Path.Combine(runDirectory, "redis-start.log"),
            ct
        );
        redisEndpoint = await Commands.RunAsync(
            "docker",
            Compose("port", "location", "6379"),
            Path.Combine(runDirectory, "redis-port.log"),
            ct
        );
        var zoneA = await StartNodeAsync("zone-a", "MyGame.Zone", true, ct);
        ZoneA = zoneA.Process;
        await StartNodeAsync("zone-b", "MyGame.Zone", true, ct);
        await StartNodeAsync("instance", "MyGame.Instance", true, ct);
        var session = await StartNodeAsync("session", "MyGame.Session", true, ct);
        Endpoint =
            session.Ready.StreamEndpoint
            ?? throw new InvalidOperationException("Session stream endpoint was not published.");
    }

    private async Task<(ManagedProcess Process, NodeReady Ready)> StartNodeAsync(
        string configName,
        string assembly,
        bool bootstrap,
        CancellationToken ct
    )
    {
        var settings =
            JsonSerializer.Deserialize<NodeSettings>(
                await File.ReadAllTextAsync(
                    Path.Combine(root, "server", "config", configName + ".json"),
                    ct
                )
            ) ?? throw new InvalidOperationException($"Invalid {configName} configuration.");
        settings = settings with
        {
            ZoneCapacity = configName == "zone-b" ? GameNames.ZoneCount - 1 : settings.ZoneCapacity,
            RedisEndpoint = redisEndpoint,
            KeyPrefix = project + ":",
            MeshEndpoint = "tcp://127.0.0.1:0",
            StreamEndpoint = settings.StreamEndpoint is null ? null : "ws://127.0.0.1:0",
            Bootstrap = bootstrap,
        };
        var suffix = bootstrap ? configName : configName + "-replacement";
        var config = Path.Combine(runDirectory, suffix + ".json");
        await File.WriteAllTextAsync(config, JsonSerializer.Serialize(settings), ct);
        var dll = Path.Combine(
            root,
            "server",
            assembly,
            "bin",
            "Debug",
            "net8.0",
            assembly + ".dll"
        );
        var node = new ManagedProcess("dotnet", [dll, "--config", config], runDirectory, suffix);
        nodes.Add(node);
        var ready =
            JsonSerializer.Deserialize<NodeReady>(
                await node.WaitLineAsync(NodeProtocol.ReadyPrefix, ct)
            ) ?? throw new InvalidOperationException($"Invalid readiness for {configName}.");
        if (ready.Id != settings.Id || ready.MeshEndpoint.EndsWith(":0", StringComparison.Ordinal))
            throw new InvalidOperationException($"Incorrect listener readiness for {configName}.");
        return (node, ready);
    }

    public async Task ReplaceZoneAAsync(CancellationToken ct)
    {
        await ZoneA.StopAsync();
        var replacement = await StartNodeAsync("zone-a", "MyGame.Zone", false, ct);
        ZoneA = replacement.Process;
    }

    public async ValueTask DisposeAsync()
    {
        List<Exception> failures = [];
        foreach (var node in Enumerable.Reverse(nodes))
        {
            try
            {
                await node.DisposeAsync();
            }
            catch (Exception error)
            {
                failures.Add(error);
            }
        }
        if (composeStarted)
        {
            try
            {
                await Commands.RunAsync(
                    "docker",
                    Compose("logs", "--no-color"),
                    Path.Combine(runDirectory, "redis.log"),
                    CancellationToken.None
                );
            }
            catch (Exception error)
            {
                failures.Add(error);
            }
            try
            {
                await Commands.RunAsync(
                    "docker",
                    Compose("down", "--volumes", "--remove-orphans"),
                    Path.Combine(runDirectory, "redis-stop.log"),
                    CancellationToken.None
                );
            }
            catch (Exception error)
            {
                failures.Add(error);
            }
        }
        if (failures.Count > 0)
            throw new AggregateException("Run cleanup failed.", failures);
    }
}
