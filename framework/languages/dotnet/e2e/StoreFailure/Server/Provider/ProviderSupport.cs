using System.Collections.Concurrent;
using System.Diagnostics;

namespace StoreFailure.Server.Provider;

using Zlink.Framework.E2E.Configuration;

internal sealed class FaultState
{
    public string Mode { get; set; } = "none";
}

internal sealed class EvidenceStore
{
    private readonly ConcurrentQueue<string> _entries = new();
    private readonly object _fileGate = new();
    private readonly string? _filePath;
    private readonly object _waiterGate = new();
    private readonly List<TaskCompletionSource> _waiters = new();

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
        SignalWaiters();
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
        Func<string[], bool> predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var elapsed = Stopwatch.StartNew();
        while (true)
        {
            var waiter = AddWaiter();
            var snapshot = Snapshot();
            if (predicate(snapshot))
            {
                RemoveWaiter(waiter);
                return snapshot;
            }

            var remaining = timeout - elapsed.Elapsed;
            if (remaining <= TimeSpan.Zero)
            {
                RemoveWaiter(waiter);
                throw new TimeoutException("Timed out waiting for resilience lifecycle evidence.");
            }

            try
            {
                await waiter.Task.WaitAsync(remaining, cancellationToken);
            }
            catch
            {
                RemoveWaiter(waiter);
                throw;
            }
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

    private TaskCompletionSource AddWaiter()
    {
        var waiter = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_waiterGate)
        {
            _waiters.Add(waiter);
        }

        return waiter;
    }

    private void RemoveWaiter(TaskCompletionSource waiter)
    {
        lock (_waiterGate)
        {
            _waiters.Remove(waiter);
        }
    }

    private void SignalWaiters()
    {
        TaskCompletionSource[] waiters;
        lock (_waiterGate)
        {
            waiters = _waiters.ToArray();
            _waiters.Clear();
        }

        foreach (var waiter in waiters) waiter.TrySetResult();
    }
}

internal sealed record ServerOptions(
    string Role,
    string Rid,
    string HttpUrl,
    string LogDir,
    int Weight,
    int LocationHeartbeatMs,
    int LocationLeaseTtlMs,
    int LocationPollingMs,
    int LocationGraceMs,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    string? ChannelEndpoint = null,
    string? EvidenceFile = null)
{
    public static ServerOptions Parse(string[] args, string defaultRole)
        => E2eConfiguration.Load<ServerOptions>(args) with { Role = defaultRole };
}
