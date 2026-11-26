using System.Text.Json;

namespace MyGame.E2E;

internal sealed record ServerEndpoints(string[] Streams, string? Api);

internal static class ServerHost
{
    public static bool IsRunning(string root)
    {
        var path = Path.Combine(root, ".logs", "server.json");
        if (!File.Exists(path))
            return false;
        try
        {
            using var lease = Lock(root);
            return false;
        }
        catch (IOException)
        {
            return File.Exists(path);
        }
    }

    public static ServerEndpoints Read(string root)
    {
        if (!IsRunning(root))
            throw new InvalidOperationException(
                "Start the server with run-server before attaching."
            );
        using var state = File.Open(
            Path.Combine(root, ".logs", "server.json"),
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read | FileShare.Delete
        );
        return JsonSerializer.Deserialize<ServerEndpoints>(state)
            ?? throw new InvalidDataException("Invalid .logs/server.json.");
    }

    public static async Task RunAsync(
        string root,
        Func<CancellationToken, Task<ServerEndpoints>> start,
        Func<CancellationToken, Task> wait,
        Func<ValueTask> stop,
        TimeSpan startupTimeout,
        CancellationToken ct
    )
    {
        var logs = Path.Combine(root, ".logs");
        Directory.CreateDirectory(logs);
        using var lease = Lock(root);
        var state = Path.Combine(logs, "server.json");
        var temporary = state + ".tmp";
        File.Delete(state);
        try
        {
            using var startup = CancellationTokenSource.CreateLinkedTokenSource(ct);
            startup.CancelAfter(startupTimeout);
            var endpoints = await start(startup.Token);
            await File.WriteAllTextAsync(temporary, JsonSerializer.Serialize(endpoints), ct);
            File.Move(temporary, state);
            foreach (var endpoint in endpoints.Streams)
                Console.WriteLine($"Server ready: {endpoint} (game clients connect here)");
            if (endpoints.Api is { } api)
                Console.WriteLine($"API endpoint: {api}");
            await wait(ct);
        }
        finally
        {
            try
            {
                File.Delete(temporary);
                File.Delete(state);
            }
            finally
            {
                Console.WriteLine("Stopping servers...");
                await stop();
            }
        }
    }

    private static FileStream Lock(string root) =>
        File.Open(
            Path.Combine(root, ".logs", "server.lock"),
            FileMode.OpenOrCreate,
            FileAccess.Write,
            FileShare.None
        );
}
