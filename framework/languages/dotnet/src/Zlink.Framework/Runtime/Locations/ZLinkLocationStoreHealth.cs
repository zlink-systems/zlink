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
    private readonly object _gate = new();
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

    internal void ReportSuccess(string source)
    {
        lock (_gate)
        {
            if (_failures.Remove(source))
                _recoveryGeneration++;
            _lastSuccessAt = _time.GetUtcNow();
        }
        Changed?.Invoke();
    }

    internal void ReportFailure(string source, Exception error)
    {
        lock (_gate)
        {
            _failures[source] = error.Message;
            _lastFailureAt = _time.GetUtcNow();
        }
        Changed?.Invoke();
    }

    internal Snapshot GetSnapshot()
    {
        lock (_gate)
        {
            return new Snapshot(
                _failures.Count == 0,
                _lastSuccessAt,
                _lastFailureAt,
                _failures.Count == 0
                    ? null
                    : string.Join("; ", _failures.OrderBy(static pair => pair.Key)
                        .Select(static pair => $"{pair.Key}: {pair.Value}")));
        }
    }

    internal long RecoveryGeneration
    {
        get
        {
            lock (_gate)
                return _recoveryGeneration;
        }
    }

    internal readonly record struct Snapshot(
        bool Healthy,
        DateTimeOffset? LastSuccessAt,
        DateTimeOffset? LastFailureAt,
        string? LastError);
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
            health?.ReportSuccess(source);
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
            health?.ReportFailure(source, failure);
            ZLinkRuntimeMetrics.RecordLocationStoreError("read");
            throw failure;
        }
        catch (Exception error)
        {
            health?.ReportFailure(source, error);
            ZLinkRuntimeMetrics.RecordLocationStoreError("read");
            throw;
        }
    }
}
