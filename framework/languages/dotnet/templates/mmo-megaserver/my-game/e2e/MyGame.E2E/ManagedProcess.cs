using System.Diagnostics;
using System.Threading.Channels;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class ManagedProcess : IAsyncDisposable
{
    private static readonly TimeSpan StartupTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(20);
    private readonly Process process;
    private readonly Task stdout;
    private readonly Task stderr;
    private readonly Channel<string> lines = Channel.CreateUnbounded<string>();
    private bool stopped;

    public ManagedProcess(
        string file,
        IEnumerable<string> arguments,
        string logDirectory,
        string name
    )
    {
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
                true
            );
            stderr = ReadAsync(
                process.StandardError,
                Path.Combine(logDirectory, name + ".err.log"),
                false
            );
        }
        catch
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
            process.WaitForExit();
            process.Dispose();
            throw;
        }
    }

    private async Task ReadAsync(StreamReader reader, string path, bool publish)
    {
        await using var log = new StreamWriter(path);
        try
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                await log.WriteLineAsync(line);
                await log.FlushAsync();
                if (
                    publish
                    && (
                        line.StartsWith(NodeProtocol.ReadyPrefix, StringComparison.Ordinal)
                        || line == NodeProtocol.RelocationRequested
                        || line == NodeProtocol.Relocated
                        || line == NodeProtocol.SafeToShutdown
                        || line == NodeProtocol.Stopped
                    )
                )
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
            prefix == NodeProtocol.SafeToShutdown ? NodeProtocol.SafeObservation : StartupTimeout
        );
        await foreach (var line in lines.Reader.ReadAllAsync(deadline.Token))
            if (line.StartsWith(prefix, StringComparison.Ordinal))
                return line[prefix.Length..];
        throw new InvalidOperationException($"Process {process.Id} exited before '{prefix}'.");
    }

    public Task CommandAsync(string command) => process.StandardInput.WriteLineAsync(command);

    public async Task StopAsync()
    {
        if (stopped)
            return;
        List<Exception> failures = [];
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
                failures.Add(error);
                Console.Error.WriteLine("Process failure before cleanup: " + error);
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync();
                    failures.Add(
                        new InvalidOperationException(
                            $"Node PID {process.Id} required forced termination."
                        )
                    );
                }
            }
        }
        try
        {
            await Task.WhenAll(stdout, stderr);
        }
        catch (Exception error)
        {
            failures.Add(error);
        }
        stopped = true;
        if (process.ExitCode != 0)
            failures.Add(
                new InvalidOperationException($"Node PID {process.Id} exited {process.ExitCode}.")
            );
        if (failures.Count > 0)
            throw new AggregateException($"Node PID {process.Id} cleanup failed.", failures);
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            await StopAsync();
        }
        catch (Exception failure)
        {
            Console.Error.WriteLine("Process failure before dispose: " + failure);
            throw;
        }
        finally
        {
            process.Dispose();
        }
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
            Console.Error.WriteLine("Command failure before cleanup: " + cancellation);
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
