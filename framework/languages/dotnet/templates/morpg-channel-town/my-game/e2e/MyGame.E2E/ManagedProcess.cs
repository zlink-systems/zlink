using System.Diagnostics;
using System.Threading.Channels;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class ManagedProcess : IAsyncDisposable
{
    private static readonly TimeSpan StartupTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(20);
    private readonly RunOutcome outcome;
    private readonly Process process;
    private readonly string name;
    private readonly bool foreground;
    private readonly Task stdout;
    private readonly Task stderr;
    private readonly Channel<string> lines = Channel.CreateUnbounded<string>();
    private bool stopped;

    public ManagedProcess(
        RunOutcome outcome,
        string file,
        IEnumerable<string> arguments,
        string logDirectory,
        string name,
        bool foreground = false
    )
    {
        this.outcome = outcome;
        this.name = name;
        this.foreground = foreground;
        Console.WriteLine($"[{name}] Starting");
        var start = new ProcessStartInfo(file)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (var argument in arguments)
            start.ArgumentList.Add(argument);
        process =
            Process.Start(start) ?? throw new InvalidOperationException($"Could not start {name}.");
        try
        {
            File.WriteAllText(Path.Combine(logDirectory, name + ".pid"), process.Id.ToString());
            stdout = ReadAsync(
                process.StandardOutput,
                Path.Combine(logDirectory, name + ".out.log"),
                true,
                Path.Combine(logDirectory, name + ".flow.log")
            );
            stderr = ReadAsync(
                process.StandardError,
                Path.Combine(logDirectory, name + ".err.log"),
                false,
                Path.Combine(logDirectory, name + ".flow.log")
            );
        }
        catch
        {
            outcome.Cleanup(() =>
            {
                if (!process.HasExited)
                    process.Kill(entireProcessTree: true);
            });
            outcome.Cleanup(process.WaitForExit);
            outcome.Cleanup(process.Dispose);
            throw;
        }
    }

    private async Task ReadAsync(StreamReader reader, string path, bool publish, string flowPath)
    {
        await using var log = new StreamWriter(path) { AutoFlush = true };
        await using var flow = publish ? null : new StreamWriter(flowPath) { AutoFlush = true };
        try
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                await log.WriteLineAsync(line);
                if (
                    flow is not null
                    && (
                        line.Contains("event=zlink.message_flow", StringComparison.Ordinal)
                        || line.Contains("event=zlink.dispatch_error", StringComparison.Ordinal)
                    )
                )
                {
                    await flow.WriteLineAsync(line);
                    if (foreground && line.Contains("phase=dispatched", StringComparison.Ordinal))
                    {
                        const string packetField = " packet=";
                        var field = line.IndexOf(packetField, StringComparison.Ordinal);
                        if (field >= 0)
                        {
                            var packet = line[(field + packetField.Length)..].Split(' ', 2)[0];
                            if (!packet.StartsWith('$'))
                                Console.WriteLine($"[{name}] Handled {packet}");
                        }
                    }
                }
                if (publish && NodeProtocol.IsMarker(line))
                    await lines.Writer.WriteAsync(line);
            }
        }
        finally
        {
            if (publish)
                lines.Writer.TryComplete();
        }
    }

    public async Task<string> WaitLineAsync(string prefix, CancellationToken ct)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(
            prefix == NodeProtocol.SafeToShutdown ? LifecycleLimits.Safe : StartupTimeout
        );
        await foreach (var line in lines.Reader.ReadAllAsync(deadline.Token))
            if (line.StartsWith(prefix, StringComparison.Ordinal))
                return line[prefix.Length..];
        throw new InvalidOperationException($"Process {process.Id} exited before '{prefix}'.");
    }

    public void Ready() => Console.WriteLine($"[{name}] Ready");

    public async Task<string> WaitForExitAsync(CancellationToken ct)
    {
        await process.WaitForExitAsync(ct);
        return name;
    }

    public Task CommandAsync(string command) => process.StandardInput.WriteLineAsync(command);

    public ValueTask StopAsync() => outcome.CleanupAsync(StopCoreAsync);

    private async ValueTask StopCoreAsync()
    {
        if (stopped)
            return;
        if (!process.HasExited)
        {
            using var deadline = new CancellationTokenSource(ShutdownTimeout);
            try
            {
                await CommandAsync(NodeProtocol.Shutdown);
                await WaitLineAsync(NodeProtocol.Stopped, deadline.Token);
                await process.WaitForExitAsync(deadline.Token);
            }
            catch (Exception error)
            {
                outcome.Record(error);
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync();
                    outcome.Record(
                        new InvalidOperationException(
                            $"Node PID {process.Id} required forced termination."
                        )
                    );
                }
            }
        }
        await outcome.CleanupAsync(async () => await stdout);
        await outcome.CleanupAsync(async () => await stderr);
        stopped = true;
        Console.WriteLine($"[{name}] Stopped (exit {process.ExitCode})");
        if (process.ExitCode != 0)
            outcome.Record(
                new InvalidOperationException($"Node PID {process.Id} exited {process.ExitCode}.")
            );
    }

    public async ValueTask DisposeAsync()
    {
        await StopAsync();
        outcome.Cleanup(process.Dispose);
    }
}

internal static class Commands
{
    private static readonly TimeSpan CommandTimeout = TimeSpan.FromSeconds(60);

    public static async Task<string> RunAsync(
        string file,
        IEnumerable<string> arguments,
        string logPath,
        CancellationToken ct
    )
    {
        var start = new ProcessStartInfo(file)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (var argument in arguments)
            start.ArgumentList.Add(argument);
        using var process =
            Process.Start(start) ?? throw new InvalidOperationException($"Could not start {file}.");
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(CommandTimeout);
        OperationCanceledException? canceled = null;
        try
        {
            await process.WaitForExitAsync(deadline.Token);
        }
        catch (OperationCanceledException cancellation)
        {
            process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync();
            canceled = cancellation;
        }
        var output = await stdout;
        var error = await stderr;
        await File.WriteAllTextAsync(logPath, output + error, CancellationToken.None);
        if (canceled is not null)
            throw new OperationCanceledException(
                $"{file} was canceled (log: {logPath}).",
                canceled,
                ct
            );
        if (process.ExitCode != 0)
            throw new InvalidOperationException(
                $"{file} exited {process.ExitCode}: {error.Trim()} (log: {logPath})"
            );
        return output.Trim();
    }
}
