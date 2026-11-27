using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class MatchRun(
    string root,
    RunOutcome outcome,
    string runDirectory,
    bool foreground = false,
    int roomCapacity = MatchRun.DefaultRoomCapacity
) : IAsyncDisposable
{
    public RunOutcome Outcome { get; } = outcome;
    internal const int DefaultRoomCapacity = 1;
    internal const int LobbyRoomCapacity = 2;
    private ManagedProcess? lobbyProcess;
    private readonly List<ManagedProcess> nodes = [];
    private readonly string project = "my-game-e2e-" + Guid.NewGuid().ToString("N");
    private readonly string? dockerProject = Environment.GetEnvironmentVariable(
        "MY_GAME_DOCKER_PROJECT"
    );
    private readonly List<string> containers = [];
    private bool composeStarted;
    private string redisEndpoint = "";
    private readonly Dictionary<string, (string Config, ManagedProcess Process)> matches = [];
    public string Endpoint { get; private set; } = "";
    public string LobbyEndpoint { get; private set; } = "";

    public ServerEndpoints PublishedEndpoints => new([Endpoint], LobbyEndpoint);

    public void Attach(ServerEndpoints endpoints)
    {
        Endpoint = endpoints.Streams.Single();
        LobbyEndpoint = endpoints.Api ?? throw new InvalidDataException("Missing lobby endpoint.");
    }

    public async Task WaitForExitAsync(CancellationToken ct)
    {
        var exit = await Task.WhenAny(nodes.Select(node => node.WaitForExitAsync(ct)));
        throw new InvalidOperationException($"{await exit} exited while the server was running.");
    }

    private string[] Compose(params string[] arguments) =>
        ["compose", "-f", Path.Combine(root, "docker-compose.yml"), "-p", project, .. arguments];

    public async Task StartAsync(CancellationToken ct)
    {
        Directory.CreateDirectory(runDirectory);
        if (dockerProject is null)
        {
            Console.WriteLine("[redis] Starting");
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
            Console.WriteLine("[redis] Ready");
        }
        else
            redisEndpoint = "location:6379";
        foreach (var configName in new[] { "match-a", "match-b" })
        {
            var table = await StartNodeAsync(configName, "MyGame.Match", false, ct);
            matches.Add(table.Ready.Id, (configName, table.Process));
        }
        var session = await StartNodeAsync("session", "MyGame.Session", false, ct);
        Endpoint =
            session.Ready.StreamEndpoint
            ?? throw new InvalidOperationException("Session stream endpoint was not published.");
        var lobby = await StartNodeAsync("lobby", "MyGame.Lobby", false, ct);
        lobbyProcess = lobby.Process;
        LobbyEndpoint =
            lobby.Ready.HttpEndpoint
            ?? throw new InvalidOperationException("HTTP endpoint was not published.");
    }

    private async Task<(ManagedProcess Process, HttpNodeReady Ready)> StartNodeAsync(
        string configName,
        string assembly,
        bool replacement,
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
            RoomCapacity = foreground ? settings.RoomCapacity : roomCapacity,
            KeyPrefix = project + ":",
            MeshEndpoint = "tcp://127.0.0.1:0",
            StreamEndpoint = settings.StreamEndpoint is null ? null : "ws://127.0.0.1:0",
            SessionEndpoint = settings.HttpEndpoint is null ? null : Endpoint,
        };
        var suffix = configName + (replacement ? "-replacement" : "");
        var config = Path.Combine(runDirectory, suffix + ".json");
        await File.WriteAllTextAsync(config, JsonSerializer.Serialize(settings), ct);
        ManagedProcess node;
        if (dockerProject is not null)
        {
            var name = dockerProject + "-" + suffix;
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
                Outcome,
                "docker",
                ["start", "-a", "-i", name],
                runDirectory,
                suffix
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
            var command = new[] { "dotnet", dll, "--config", config };
            // Handle terminal interrupts in the runner and stop the node through stdin.
            if (foreground && !OperatingSystem.IsWindows())
                command = ["sh", "-c", "trap '' INT; exec \"$@\"", "node", .. command];
            node = new ManagedProcess(
                Outcome,
                command[0],
                command[1..],
                runDirectory,
                suffix,
                foreground
            );
        }
        nodes.Add(node);
        var ready =
            JsonSerializer.Deserialize<HttpNodeReady>(
                await node.WaitLineAsync(NodeProtocol.ReadyPrefix, ct),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true }
            ) ?? throw new InvalidOperationException($"Invalid readiness for {configName}.");
        if (ready.Id != settings.Id || ready.MeshEndpoint.EndsWith(":0", StringComparison.Ordinal))
            throw new InvalidOperationException($"Incorrect listener readiness for {configName}.");
        node.Ready();
        return (node, ready);
    }

    public async Task RestartLobbyAsync(CancellationToken ct)
    {
        await (
            lobbyProcess ?? throw new InvalidOperationException("Lobby has not started.")
        ).StopAsync();
        var restarted = await StartNodeAsync("lobby", "MyGame.Lobby", true, ct);
        lobbyProcess = restarted.Process;
        LobbyEndpoint =
            restarted.Ready.HttpEndpoint
            ?? throw new InvalidOperationException("Restarted HTTP endpoint was not published.");
    }

    public async Task DrainAsync(string nodeId, CancellationToken ct)
    {
        var node = matches[nodeId].Process;
        await node.CommandAsync(NodeProtocol.Drain);
        await node.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
        await node.WaitLineAsync(NodeProtocol.Relocated, ct);
        await node.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
    }

    public async Task ReplaceAsync(string nodeId, CancellationToken ct)
    {
        await matches[nodeId].Process.StopAsync();
        var replacement = await StartNodeAsync(matches[nodeId].Config, "MyGame.Match", true, ct);
        matches[nodeId] = (matches[nodeId].Config, replacement.Process);
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
                Outcome.Record(error);
            }
        }
        if (composeStarted)
        {
            Console.WriteLine("[redis] Stopping");
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
                Outcome.Record(error);
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
                Outcome.Record(error);
            }
        }
    }
}
