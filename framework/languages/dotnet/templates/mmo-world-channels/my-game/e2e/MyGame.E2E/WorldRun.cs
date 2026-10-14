using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class WorldRun(string root) : IAsyncDisposable
{
    private readonly List<ManagedProcess> nodes = [];
    private readonly string project = "my-game-e2e-" + Guid.NewGuid().ToString("N");
    private readonly string runDirectory = Path.Combine(root, ".logs", "run-" + Guid.NewGuid().ToString("N"));
    private readonly Dictionary<string, string> redis = [];
    private readonly Dictionary<string, string> configurations = [];
    private bool composeStarted;
    public Dictionary<string, ManagedProcess> Channels { get; } = [];
    public Dictionary<string, string> Endpoints { get; } = [];
    public string AccountsEndpoint { get; private set; } = "";
    private static readonly JsonSerializerOptions ReadyJson = new() { PropertyNameCaseInsensitive = true };
    private string[] Compose(params string[] arguments) => ["compose", "-f", Path.Combine(root, "docker-compose.yml"), "-p", project, .. arguments];
    public async Task StartAsync(CancellationToken ct)
    {
        Directory.CreateDirectory(runDirectory);
        Console.WriteLine($"Run logs: {runDirectory}");
        composeStarted = true;
        await Commands.RunAsync("docker", Compose("up", "-d", "--wait", "--wait-timeout", "30"), Path.Combine(runDirectory, "redis-start.log"), ct);
        foreach (var world in new[] { "A", "B" })
            redis.Add(world, await Commands.RunAsync("docker", Compose("port", "db" + world, "6379"), Path.Combine(runDirectory, "db" + world + "-port.log"), ct));
        foreach (var id in new[] { "channelA1", "channelA2", "channelB" })
            Channels.Add(id, (await StartNodeAsync(id, "MyGame.Channel", false, ct)).Process);
        foreach (var world in new[] { "A", "B" })
        {
            var session = await StartNodeAsync("session" + world, "MyGame.Session", false, ct);
            Endpoints.Add(world, session.Ready.StreamEndpoint ?? throw new InvalidDataException("Session endpoint is missing."));
        }
        var accounts = await StartNodeAsync("accounts", "MyGame.Accounts", false, ct);
        AccountsEndpoint = accounts.Ready.HttpEndpoint ?? throw new InvalidDataException("HTTP endpoint is missing.");
    }
    private async Task<(ManagedProcess Process, NodeReady Ready)> StartNodeAsync(string id, string assembly, bool replacement, CancellationToken ct)
    {
        var config = replacement ? configurations[id] : Path.Combine(runDirectory, id + ".json");
        if (!replacement)
        {
            var settings = NodeSettings.Load(["--config", Path.Combine(root, "server", "config", id + ".json")]);
            settings = settings with
            {
                RedisEndpoint = redis[settings.WorldId],
                KeyPrefix = project + ":world:" + settings.WorldId + ":",
                MeshEndpoint = "tcp://127.0.0.1:0",
                StreamEndpoint = settings.StreamEndpoint is null ? null : "ws://127.0.0.1:0",
                HttpEndpoint = settings.HttpEndpoint is null ? null : "http://127.0.0.1:0",
                Worlds = settings.Worlds is null ? null : settings.Worlds.Select(world => world with { SessionEndpoint = Endpoints[world.WorldId] }).ToArray()
            };
            await File.WriteAllTextAsync(config, JsonSerializer.Serialize(settings), ct);
            configurations.Add(id, config);
        }
        var name = replacement ? id + "-replacement" : id;
        var dll = Path.Combine(root, "server", assembly, "bin", "Debug", "net8.0", assembly + ".dll");
        var node = new ManagedProcess("dotnet", [dll, "--config", config], runDirectory, name);
        nodes.Add(node);
        var ready = JsonSerializer.Deserialize<NodeReady>(await node.WaitLineAsync(NodeProtocol.ReadyPrefix, ct), ReadyJson)
            ?? throw new InvalidDataException("Invalid readiness JSON.");
        if (ready.Id != id || ready.MeshEndpoint.EndsWith(":0", StringComparison.Ordinal) ||
            ready.StreamEndpoint?.EndsWith(":0", StringComparison.Ordinal) == true ||
            ready.HttpEndpoint?.EndsWith(":0", StringComparison.Ordinal) == true)
            throw new InvalidDataException("Listener readiness contains an incorrect identity or an unbound endpoint.");
        return (node, ready);
    }
    public async Task ReplaceChannelAsync(string id, CancellationToken ct)
    {
        await Channels[id].StopAsync();
        Channels[id] = (await StartNodeAsync(id, "MyGame.Channel", true, ct)).Process;
    }
    public async ValueTask DisposeAsync()
    {
        List<Exception> failures = [];
        foreach (var node in Enumerable.Reverse(nodes))
        {
            try { await node.DisposeAsync(); }
            catch (Exception error) { failures.Add(error); }
        }
        if (composeStarted)
        {
            try { await Commands.RunAsync("docker", Compose("logs", "--no-color"), Path.Combine(runDirectory, "redis.log"), CancellationToken.None); }
            catch (Exception error) { failures.Add(error); }
            try { await Commands.RunAsync("docker", Compose("down", "--volumes", "--remove-orphans"), Path.Combine(runDirectory, "redis-stop.log"), CancellationToken.None); }
            catch (Exception error) { failures.Add(error); }
        }
        if (failures.Count > 0) throw new AggregateException("Run cleanup failed.", failures);
    }
}
