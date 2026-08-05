using System.Collections.Concurrent;
using Zlink.Framework.Contracts.Configuration;

namespace LocationMessaging.Server.Consumer;

internal sealed class ConnectionEvidence
{
    private readonly ConcurrentQueue<string> _entries = new();
    // A pulse completed on every Add and swapped for a fresh one, so EVERY
    // concurrent waiter wakes — a counted semaphore hands one release to one
    // waiter and silently starves the rest.
    private readonly object _pulseGate = new();
    private TaskCompletionSource<bool> _pulse =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

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
    }

    public async Task<string[]> WaitAsync(
        string contains,
        int afterCount,
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

            var snapshot = _entries.ToArray();
            if (snapshot.Skip(Math.Clamp(afterCount, 0, snapshot.Length))
                .Any(line => line.Contains(contains, StringComparison.Ordinal))) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero
                || await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken)) != pulseTask)
                throw new TimeoutException(
                    $"Connection evidence containing '{contains}' did not arrive within {timeout}.");
        }
    }
}

internal sealed class MeshConnectionEventObserver(
    ConnectionEvidence evidence,
    IZLinkRouteMeshRuntime meshRuntime,
    Configuration.ConsumerOptions options,
    ILogger<MeshConnectionEventObserver> logger)
    : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var previous = new HashSet<string>(StringComparer.Ordinal);
        await foreach (var status in meshRuntime
                           .ObserveAsync(options.MeshName, stoppingToken)
                           .ConfigureAwait(false))
        {
            var current = status.Status.Peers
                .Where(static peer => peer.State == ZLinkPeerState.Ready)
                .Select(static peer => peer.NodeRid.ToString())
                .ToHashSet(StringComparer.Ordinal);
            foreach (var peer in current.Except(previous))
            {
                Add("ConnectionReady", peer, status.Status.Sequence);
            }
            foreach (var peer in previous.Except(current))
            {
                Add("Disconnected", peer, status.Status.Sequence);
            }
            previous = current;
        }
    }

    private void Add(string kind, string peer, ulong sequence)
    {
        var entry =
            $"monitor-mesh|source=profile|kind={kind}"
            + $"|remote=|routing={peer}|sequence={sequence}";
        evidence.Add(entry);
        logger.LogInformation("location connection evidence: {Entry}", entry);
    }
}
