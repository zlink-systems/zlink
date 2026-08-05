using System.Collections.Concurrent;
using Zlink.Framework.LocationProvider;

namespace SpotActorTransfer.ActorNode;

internal sealed class ActorCleanupGateStore(EvidenceStore evidence)
{
    private readonly ConcurrentDictionary<string, CleanupGate> _gates =
        new(StringComparer.Ordinal);

    public bool Arm(string actorId, string scenario)
    {
        if (!_gates.IsEmpty)
            return false;

        return _gates.TryAdd(actorId, new CleanupGate(scenario, actorId));
    }

    public bool AllowAttempt(string actorId) =>
        _gates.TryGetValue(actorId, out var gate)
        && gate.AllowAttempt.TrySetResult();

    public bool Release(string actorId) =>
        _gates.TryGetValue(actorId, out var gate)
        && gate.Release.TrySetResult();

    public async ValueTask WaitBeforeDeleteAsync(
        CancellationToken cancellationToken)
    {
        var pair = _gates.SingleOrDefault();
        var gate = pair.Value;
        if (gate is null)
            return;

        await gate.AllowAttempt.Task.WaitAsync(cancellationToken);
        if (Interlocked.Exchange(ref gate.AttemptObserved, 1) == 0)
            evidence.Add(
                gate.Scenario,
                gate.ActorId,
                "source_cleanup_attempt",
                "opaque-delete-batch");

        await gate.Release.Task.WaitAsync(cancellationToken);
        if (_gates.TryRemove(pair.Key, out _))
            evidence.Add(
                gate.Scenario,
                gate.ActorId,
                "source_cleanup_completed",
                "opaque-delete-batch");
    }

    private sealed class CleanupGate(string scenario, string actorId)
    {
        public string Scenario { get; } = scenario;
        public string ActorId { get; } = actorId;
        public TaskCompletionSource AllowAttempt { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int AttemptObserved;
    }
}

/// <summary>
/// Delays the next isolated conditional delete batch without interpreting
/// Framework-owned keys or payloads.
/// </summary>
internal sealed class CleanupGatedLocationStore(
    IZLinkLocationStore inner,
    ActorCleanupGateStore cleanupGates) :
    IZLinkLocationStore,
    IAsyncDisposable
{
    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default) =>
        inner.ReadAsync(key, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        if (request.Mutations.Any(static mutation =>
                mutation is ZLinkStoreMutation.Delete))
            await cleanupGates.WaitBeforeDeleteAsync(cancellationToken);

        return await inner.WriteAsync(request, cancellationToken);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default) =>
        inner.ScanAsync(request, cancellationToken);

    public async ValueTask DisposeAsync()
    {
        switch (inner)
        {
            case IAsyncDisposable asyncDisposable:
                await asyncDisposable.DisposeAsync();
                break;
            case IDisposable disposable:
                disposable.Dispose();
                break;
        }
    }
}
