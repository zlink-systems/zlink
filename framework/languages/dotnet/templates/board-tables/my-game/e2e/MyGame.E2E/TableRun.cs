using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class TableRun(
    string root,
    int tableCapacity,
    RunOutcome outcome,
    string runDirectory,
    bool foreground = false,
    ServerEndpoints? persistenceSource = null
) : IAsyncDisposable
{
    public static readonly TimeSpan RunDeadline = TimeSpan.FromMinutes(3);
    private readonly List<ManagedProcess> nodes = [];
    private readonly string project = "my-game-e2e-" + Guid.NewGuid().ToString("N");
    private readonly string? dockerProject = Environment.GetEnvironmentVariable(
        "MY_GAME_DOCKER_PROJECT"
    );
    private readonly List<string> containers = [];
    private bool composeStarted;
    private string redisEndpoint = "";
    private readonly Dictionary<string, (string Config, ManagedProcess Process)> tables = [];
    public string Endpoint { get; private set; } = "";
    private string? apiEndpoint;
    private string WorldPrefix => project + ":";
    private string PlayerRecordsPrefix => persistenceSource?.PlayerRecordsKeyPrefix ?? WorldPrefix;
    public ServerEndpoints Endpoints =>
        new([Endpoint], apiEndpoint, redisEndpoint, PlayerRecordsPrefix);

    private string[] Compose(params string[] arguments) =>
        ["compose", "-f", Path.Combine(root, "docker-compose.yml"), "-p", project, .. arguments];

    public async Task StartAsync(CancellationToken ct)
    {
        Directory.CreateDirectory(runDirectory);
        if (persistenceSource is not null)
        {
            redisEndpoint = persistenceSource.Redis;
        }
        else if (
            dockerProject is null
            && Environment.GetEnvironmentVariable("ZLINK_REDIS_TEST_ENDPOINT")
                is { Length: > 0 } endpoint
        )
        {
            redisEndpoint = endpoint;
        }
        else if (dockerProject is null)
        {
            composeStarted = true;
            await Commands.RunAsync(
                "docker",
                Compose("up", "-d", "--wait", "--wait-timeout", "30", "location"),
                Path.Combine(runDirectory, "redis-start.log"),
                ct
            );
            redisEndpoint = await Commands.RunAsync(
                "docker",
                Compose("port", "location", "6379"),
                Path.Combine(runDirectory, "redis-port.log"),
                ct
            );
        }
        else
            redisEndpoint = "location:6379";
        var api = await StartNodeAsync("api", "MyGame.Api", false, ct);
        apiEndpoint =
            api.Ready.ApiEndpoint
            ?? throw new InvalidOperationException("API endpoint was not published.");
        if (persistenceSource is null)
            await StartNodeAsync("matchmaker", "MyGame.Matchmaker", false, ct);
        var tableConfigs = persistenceSource is null ? new[] { "table-a", "table-b" } : ["table-a"];
        foreach (var configName in tableConfigs)
        {
            var table = await StartNodeAsync(configName, "MyGame.Table", false, ct);
            tables.Add(table.Ready.Id, (configName, table.Process));
        }
        var session = await StartNodeAsync("session", "MyGame.Session", false, ct);
        Endpoint =
            session.Ready.StreamEndpoint
            ?? throw new InvalidOperationException("Session stream endpoint was not published.");
    }

    private async Task<(ManagedProcess Process, NodeReady Ready)> StartNodeAsync(
        string configName,
        string assembly,
        bool replacement,
        CancellationToken ct
    )
    {
        var sourceConfig = Path.Combine(root, "server", "config", configName + ".json");
        var settings = NodeSettings.Load(["--config", sourceConfig]);
        settings = settings with
        {
            TableCapacity = tableCapacity,
            RedisEndpoint = redisEndpoint,
            KeyPrefix = WorldPrefix,
            PlayerRecordsKeyPrefix = configName == "api" ? PlayerRecordsPrefix : null,
            MeshEndpoint = settings.MeshEndpoint is null ? null : "tcp://127.0.0.1:0",
            ApiEndpoint = configName == "api" ? "tcp://127.0.0.1:0" : null,
            StreamEndpoint = settings.StreamEndpoint is null ? null : "ws://127.0.0.1:0",
        };
        var suffix = configName + (replacement ? "-replacement" : "");
        var config = Path.Combine(runDirectory, suffix + ".json");
        await File.WriteAllTextAsync(config, JsonSerializer.Serialize(settings), ct);
        ManagedProcess node;
        if (dockerProject is not null)
        {
            var name = project + "-" + suffix;
            var image =
                Environment.GetEnvironmentVariable("MY_GAME_NODE_IMAGE")
                ?? throw new InvalidOperationException("Missing node image.");
            var runner =
                Environment.GetEnvironmentVariable("MY_GAME_RUNNER_CONTAINER")
                ?? throw new InvalidOperationException("Missing runner container name.");
            await Commands.RunAsync(
                "docker",
                [
                    "create",
                    "-i",
                    "--name",
                    name,
                    "--label",
                    "my-game.e2e=" + dockerProject,
                    "--network",
                    "container:" + runner,
                    image,
                    "/app/"
                        + assembly["MyGame.".Length..].ToLowerInvariant()
                        + "/"
                        + assembly
                        + ".dll",
                    "--config",
                    "/app/node.json",
                ],
                Path.Combine(runDirectory, suffix + "-create.log"),
                ct
            );
            containers.Add(name);
            await Commands.RunAsync(
                "docker",
                ["cp", config, name + ":/app/node.json"],
                Path.Combine(runDirectory, suffix + "-config.log"),
                ct
            );
            node = new ManagedProcess(
                outcome,
                "docker",
                ["start", "-a", "-i", name],
                runDirectory,
                suffix,
                foreground
            );
        }
        else
        {
            var dll = Path.Combine(
                root,
                "server",
                assembly,
                "bin",
                "Debug",
                "net8.0",
                assembly + ".dll"
            );
            node = new ManagedProcess(
                outcome,
                "dotnet",
                [dll, "--config", config],
                runDirectory,
                suffix,
                foreground
            );
        }
        nodes.Add(node);
        var ready =
            JsonSerializer.Deserialize<NodeReady>(
                await node.WaitLineAsync(NodeProtocol.ReadyPrefix, ct)
            ) ?? throw new InvalidOperationException($"Invalid readiness for {configName}.");
        var listener = ready.MeshEndpoint ?? ready.ApiEndpoint;
        if (
            ready.Id != settings.Id
            || listener is null
            || listener.EndsWith(":0", StringComparison.Ordinal)
        )
            throw new InvalidOperationException($"Incorrect listener readiness for {configName}.");
        node.Ready();
        return (node, ready);
    }

    public async Task ProbeFreshWorldAsync(
        Func<string, CancellationToken, Task> probe,
        CancellationToken ct
    )
    {
        await using var fresh = new TableRun(
            root,
            1,
            outcome,
            Path.Combine(runDirectory, "fresh-world")
        );
        await fresh.StartAsync(ct);
        await probe(fresh.Endpoint, ct);
    }

    public async Task WaitForExitAsync(CancellationToken ct)
    {
        var exited = await Task.WhenAny(nodes.Select(node => node.WaitForExitAsync(ct)));
        throw new InvalidOperationException($"Server node {await exited} exited.");
    }

    public async Task DrainAsync(string nodeId, CancellationToken ct)
    {
        var node = tables[nodeId].Process;
        await node.CommandAsync(NodeProtocol.Drain);
        await node.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
        await node.WaitLineAsync(NodeProtocol.Relocated, ct);
        await node.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
    }

    public async Task ReplaceAsync(string nodeId, CancellationToken ct)
    {
        await tables[nodeId].Process.StopAsync();
        var replacement = await StartNodeAsync(tables[nodeId].Config, "MyGame.Table", true, ct);
        tables[nodeId] = (tables[nodeId].Config, replacement.Process);
    }

    public async ValueTask DisposeAsync()
    {
        foreach (var node in Enumerable.Reverse(nodes))
        {
            await node.DisposeAsync();
        }
        foreach (var container in containers)
        {
            try
            {
                await Commands.RunAsync(
                    "docker",
                    ["rm", "-f", container],
                    Path.Combine(runDirectory, container + "-remove.log"),
                    CancellationToken.None
                );
            }
            catch (Exception error)
            {
                outcome.Record(error);
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
                outcome.Record(error);
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
                outcome.Record(error);
            }
        }
    }
}
