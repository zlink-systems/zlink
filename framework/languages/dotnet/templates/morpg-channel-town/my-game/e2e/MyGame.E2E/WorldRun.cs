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
    public Dictionary<string, ManagedProcess> Instances { get; } = new(StringComparer.Ordinal);
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
        await StartNodeAsync("townA", "MyGame.Town", true, ct);
        await StartNodeAsync("townB", "MyGame.Town", true, ct);
        foreach (var id in new[] { "instanceA", "instanceB" })
            Instances.Add(id, (await StartNodeAsync(id, "MyGame.Instance", true, ct)).Process);
        var session = await StartNodeAsync("session", "MyGame.Session", true, ct);
        Endpoint =
            session.Ready.StreamEndpoint
            ?? throw new InvalidOperationException("Session stream endpoint was not published.");
    }

    private async Task<(ManagedProcess Process, NodeReady Ready)> StartNodeAsync(
        string configName,
        string assembly,
        bool initial,
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
            RedisEndpoint = redisEndpoint,
            KeyPrefix = project + ":",
            MeshEndpoint = "tcp://127.0.0.1:0",
            StreamEndpoint = settings.StreamEndpoint is null ? null : "ws://127.0.0.1:0",
        };
        var suffix = initial ? configName : configName + "-replacement";
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

    public async Task ReplaceInstanceAsync(string id, CancellationToken ct)
    {
        await Instances[id].StopAsync();
        var replacement = await StartNodeAsync(id, "MyGame.Instance", false, ct);
        Instances[id] = replacement.Process;
    }

    public async ValueTask DisposeAsync()
    {
        foreach (var node in Enumerable.Reverse(nodes))
        {
            await node.DisposeAsync();
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
                RunOutcome.Record(error);
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
                RunOutcome.Record(error);
            }
        }
    }
}
