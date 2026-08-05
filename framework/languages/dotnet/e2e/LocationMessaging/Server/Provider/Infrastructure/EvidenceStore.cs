using System.Collections.Concurrent;

namespace LocationMessaging.Server.Provider.Infrastructure;

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

    public EvidenceStore(string rid, string? filePath)
    {
        _filePath = filePath;
        Rid = rid;
        if (!string.IsNullOrWhiteSpace(_filePath))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
            File.WriteAllText(_filePath, string.Empty);
        }
    }

    public string Rid { get; }

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
        Func<string, bool> predicate,
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
            if (snapshot.Any(predicate)) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero
                || await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken)) != pulseTask)
                throw new TimeoutException($"Evidence did not satisfy the predicate within {timeout}.");
        }
    }

    public async Task<string[]> WaitUntilCountAsync(
        string contains,
        int minimumCount,
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
            if (snapshot.Count(line => line.Contains(contains, StringComparison.Ordinal)) >= minimumCount)
                return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero
                || await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken)) != pulseTask)
                throw new TimeoutException(
                    $"Evidence containing '{contains}' did not reach count {minimumCount} within {timeout}.");
        }
    }

    public async Task<string[]> WaitUntilQuietAsync(
        string contains,
        TimeSpan quietPeriod,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            var before = Snapshot();
            var matchingCount = before.Count(line => line.Contains(contains, StringComparison.Ordinal));
            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining < quietPeriod)
                throw new TimeoutException(
                    $"Evidence containing '{contains}' did not remain quiet for {quietPeriod} within {timeout}.");

            await Task.Delay(quietPeriod, cancellationToken);
            var after = Snapshot();
            var updatedCount = after.Count(line => line.Contains(contains, StringComparison.Ordinal));
            if (updatedCount == matchingCount) return after;
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
