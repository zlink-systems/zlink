using System.Text.Json;

namespace MyGame.E2E;

internal static class ServerHost
{
    private sealed record State(string SessionEndpoint);

    private static string StatePath(string root) => Path.Combine(root, ".logs", "server.json");

    private static FileStream Lock(string root) =>
        File.Open(
            Path.Combine(root, ".logs", "server.lock"),
            FileMode.OpenOrCreate,
            FileAccess.Write,
            FileShare.None
        );

    public static bool IsRunning(string root)
    {
        var state = StatePath(root);
        if (!File.Exists(state))
            return false;
        try
        {
            using var lease = Lock(root);
            return false;
        }
        catch (IOException)
        {
            return File.Exists(state);
        }
    }

    public static string Endpoint(string root)
    {
        if (!IsRunning(root))
            throw new InvalidOperationException(
                "Start the server with run-server before attaching."
            );
        var state =
            JsonSerializer.Deserialize<State>(File.ReadAllText(StatePath(root)))
            ?? throw new InvalidDataException("Invalid .logs/server.json.");
        return state.SessionEndpoint;
    }

    public static async Task RunAsync(string root, TableRun run, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.Combine(root, ".logs"));
        using var lease = Lock(root);
        await using var ownedRun = run;
        var statePath = StatePath(root);
        var temporary = statePath + ".tmp";
        File.Delete(statePath);
        try
        {
            using var startup = CancellationTokenSource.CreateLinkedTokenSource(ct);
            startup.CancelAfter(TimeSpan.FromMinutes(3));
            await run.StartAsync(startup.Token);
            await File.WriteAllTextAsync(
                temporary,
                JsonSerializer.Serialize(new State(run.Endpoint)),
                ct
            );
            File.Move(temporary, statePath);
            Console.WriteLine($"Server ready: {run.Endpoint} (game clients connect here)");
            await Task.Delay(Timeout.InfiniteTimeSpan, ct);
        }
        finally
        {
            File.Delete(temporary);
            File.Delete(statePath);
            Console.WriteLine("Stopping servers...");
        }
    }
}
