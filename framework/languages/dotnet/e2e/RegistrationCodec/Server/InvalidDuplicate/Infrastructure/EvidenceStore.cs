using System.Collections.Concurrent;

namespace RegistrationCodec.Server.InvalidDuplicate.Infrastructure;

internal sealed class EvidenceStore
{
    private readonly ConcurrentQueue<string> _entries = new();
    private readonly object _fileGate = new();
    private readonly string? _filePath;
    // A pulse completed on every Add and swapped for a fresh one, so EVERY
    // concurrent waiter wakes — a counted semaphore hands one release to one
    // waiter and silently starves the rest.
    private readonly object _pulseGate = new();
    private TaskCompletionSource<bool> _pulse =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public EvidenceStore(string? filePath)
    {
        _filePath = filePath;
        if (!string.IsNullOrWhiteSpace(_filePath))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
            File.WriteAllText(_filePath, string.Empty);
        }
    }

    public void Add(string entry)
    {
        _entries.Enqueue(entry);
        TaskCompletionSource<bool> pulse;
        lock (_pulseGate)
        {
            pulse = _pulse;
            _pulse = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }

        pulse.TrySetResult(true);
        if (string.IsNullOrWhiteSpace(_filePath)) return;

        lock (_fileGate)
        {
            File.AppendAllText(_filePath, entry + Environment.NewLine);
        }
    }

    public string[] Snapshot()
    {
        return _entries.ToArray();
    }

    public async Task<string[]> WaitUntilAsync(
        Func<string[], bool> condition,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            Task pulseTask;
            lock (_pulseGate)
            {
                pulseTask = _pulse.Task;
            }

            var snapshot = Snapshot();
            if (condition(snapshot)) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero)
                throw new TimeoutException("Timed out waiting for registration codec evidence.");

            await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken));
        }
    }

    public void Clear()
    {
        while (_entries.TryDequeue(out _))
        {
        }

        if (!string.IsNullOrWhiteSpace(_filePath))
            lock (_fileGate)
            {
                File.WriteAllText(_filePath, string.Empty);
            }
    }
}