namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Aggregates store read failures by operation boundary. A successful read
/// clears only its own failure, so one healthy subsystem cannot hide an
/// outage still affecting another location capability.
/// </summary>
internal sealed class ZLinkLocationStoreHealth
{
    //  Store health timestamps are compared against the lease renewal time,
    //  which comes from the runtime's injected clock. Reading the system clock
    //  here would put the two on different timelines.
    private readonly TimeProvider _time;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<string, string> _failures = new(StringComparer.Ordinal);
    private DateTimeOffset? _lastSuccessAt;
    private DateTimeOffset? _lastFailureAt;
    private long _recoveryGeneration;

    internal event Action? Changed;

    //  The container resolves this type, so the parameterless constructor has
    //  to stay; the clock overload is for callers that already own one.
    public ZLinkLocationStoreHealth()
        : this(TimeProvider.System)
    {
    }

    internal ZLinkLocationStoreHealth(TimeProvider time)
    {
        _time = time;
    }

    internal async ValueTask ReportSuccessAsync(string source)
    {
        await _lane.RunAsync(() =>
        {
            if (_failures.Remove(source))
                _recoveryGeneration++;
            _lastSuccessAt = _time.GetUtcNow();
        }).ConfigureAwait(false);
        Changed?.Invoke();
    }

    internal async ValueTask ReportFailureAsync(string source, Exception error)
    {
        await _lane.RunAsync(() =>
        {
            _failures[source] = error.Message;
            _lastFailureAt = _time.GetUtcNow();
        }).ConfigureAwait(false);
        Changed?.Invoke();
    }

    internal Snapshot GetSnapshot()
    {
        // Public topology GetStatus signatures are synchronous. Their snapshot
        // capture must finish before returning; async runtime callers use the
        // same owner's async capture below (state ownership spec, §5).
        return AwaitStateLane(GetSnapshotAsync());
    }

    internal ValueTask<Snapshot> GetSnapshotAsync()
    {
        return _lane.RunAsync(() =>
        {
            return new Snapshot(
                _failures.Count == 0,
                _lastSuccessAt,
                _lastFailureAt,
                _failures.Count == 0
                    ? null
                    : string.Join("; ", _failures.OrderBy(static pair => pair.Key)
                        .Select(static pair => $"{pair.Key}: {pair.Value}")));
        });
    }

    internal long RecoveryGeneration
    {
        // The resolver compares this generation inside its cache-state turn,
        // and captures it before constructing a cached route. Keep that capture
        // complete before returning rather than reading ahead of the turn (§5).
        get => AwaitStateLane(_lane.RunAsync(() => _recoveryGeneration));
    }

    internal readonly record struct Snapshot(
        bool Healthy,
        DateTimeOffset? LastSuccessAt,
        DateTimeOffset? LastFailureAt,
        string? LastError);

    // These capture-only turns never acquire a caller's gate or invoke Changed;
    // queued completion uses StateLane's asynchronous continuations (§5).
    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}

internal static class ZLinkLocationStoreRead
{
    internal static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    internal static async ValueTask<T> ExecuteAsync<T>(
        ZLinkLocationStoreHealth? health,
        string source,
        CancellationToken callerToken,
        Func<CancellationToken, ValueTask<T>> read)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(callerToken);
        timeout.CancelAfter(Timeout);
        try
        {
            // WaitAsync bounds stores whose in-flight commands cannot observe
            // the token (a paused Redis holds replies indefinitely): the read
            // boundary must degrade within its timeout either way.
            var result = await read(timeout.Token).AsTask().WaitAsync(timeout.Token)
                .ConfigureAwait(false);
            if (health is not null)
                await health.ReportSuccessAsync(source).ConfigureAwait(false);
            return result;
        }
        catch (OperationCanceledException) when (callerToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException error) when (timeout.IsCancellationRequested)
        {
            var failure = new TimeoutException(
                $"Location store read '{source}' exceeded {Timeout}.",
                error);
            if (health is not null)
                await health.ReportFailureAsync(source, failure).ConfigureAwait(false);
            ZLinkRuntimeMetrics.RecordLocationStoreError("read");
            throw failure;
        }
        catch (Exception error)
        {
            if (health is not null)
                await health.ReportFailureAsync(source, error).ConfigureAwait(false);
            ZLinkRuntimeMetrics.RecordLocationStoreError("read");
            throw;
        }
    }
}
