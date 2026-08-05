using System.Collections.Concurrent;

namespace ObservabilityOps.Server.Workflow.Support;

internal sealed class WorkflowEvidenceStore
{
    private readonly ConcurrentQueue<string> _entries = new();
    private readonly SemaphoreSlim _changed = new(0);
    public void Add(string entry) { _entries.Enqueue(entry); _changed.Release(); }
    public string[] Snapshot() => _entries.ToArray();
    public async Task<string[]> WaitAsync(Func<string[], bool> predicate, TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            var snapshot = Snapshot();
            if (predicate(snapshot)) return snapshot;
            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero) throw new TimeoutException("Workflow evidence wait timed out.");
            await _changed.WaitAsync(remaining, cancellationToken);
        }
    }
}
