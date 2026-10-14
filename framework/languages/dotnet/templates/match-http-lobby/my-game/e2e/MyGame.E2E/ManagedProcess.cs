using System.Diagnostics;
using System.Threading.Channels;
using MyGame.Shared;

namespace MyGame.E2E;

internal sealed class ManagedProcess : IAsyncDisposable
{
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
            WorkingDirectory = logDirectory,
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
        deadline.CancelAfter(TimeSpan.FromSeconds(30));
        try
        {
            await foreach (var line in lines.Reader.ReadAllAsync(deadline.Token))
                if (line.StartsWith(prefix, StringComparison.Ordinal))
                    return line[prefix.Length..];
        }
        catch (OperationCanceledException error) when (!ct.IsCancellationRequested)
        {
            throw new TimeoutException(
                $"Process {process.Id} did not emit '{prefix}' before its deadline.",
                error
            );
        }
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
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            try
            {
                await CommandAsync(NodeProtocol.Shutdown);
                await process.WaitForExitAsync(deadline.Token);
            }
            catch (Exception error)
            {
                failures.Add(error);
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
        finally
        {
            process.Dispose();
        }
    }
}

internal static class Commands
{
    public static async Task<string> RunAsync(
        string file,
        IEnumerable<string> arguments,
        string logPath,
        CancellationToken ct
    )
    {
        var start = new ProcessStartInfo(file)
        {
            WorkingDirectory = Path.GetDirectoryName(logPath)!,
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
        deadline.CancelAfter(TimeSpan.FromSeconds(60));
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
