namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkBoundSessionDispatchScope : IAsyncDisposable
{
    private const int MaxDeferredOperations = 4096;
    private static readonly AsyncLocal<ZLinkBoundSessionDispatchScope?> CurrentScope = new();
    private readonly string _actorId;
    private readonly object _gate = new();
    private readonly Queue<Func<CancellationToken, ValueTask>> _deferredOperations = new();
    private readonly SemaphoreSlim _drainGate = new(1, 1);
    private readonly ZLinkBoundSessionDispatchScope? _previous;
    private bool _drained;

    private ZLinkBoundSessionDispatchScope(string actorId)
    {
        _actorId = actorId;
        _previous = CurrentScope.Value;
        CurrentScope.Value = this;
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            await DrainAsync(CancellationToken.None).ConfigureAwait(false);
        }
        finally
        {
            CurrentScope.Value = _previous;
        }
    }

    public static ZLinkBoundSessionDispatchScope Enter(string actorId)
    {
        return new ZLinkBoundSessionDispatchScope(actorId);
    }

    public static bool TryDeferClose(
        string actorId,
        Func<CancellationToken, ValueTask> closeAsync)
    {
        return TryDefer(actorId, closeAsync);
    }

    public static bool TryDefer(
        string actorId,
        Func<CancellationToken, ValueTask> operationAsync)
    {
        var scope = CurrentScope.Value;
        if (scope is null || !string.Equals(scope._actorId, actorId, StringComparison.Ordinal))
            return false;

        lock (scope._gate)
        {
            if (scope._drained) return false;
            if (scope._deferredOperations.Count >= MaxDeferredOperations)
                throw new InvalidOperationException("Bound-session deferred submit queue is full.");
            scope._deferredOperations.Enqueue(operationAsync);
            return true;
        }
    }

    public async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        await _drainGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            while (true)
            {
                Func<CancellationToken, ValueTask>? operation;
                lock (_gate)
                {
                    if (_drained) return;
                    if (!_deferredOperations.TryPeek(out operation))
                    {
                        _drained = true;
                        return;
                    }
                }

                await operation(cancellationToken).ConfigureAwait(false);
                lock (_gate)
                {
                    if (!_deferredOperations.TryDequeue(out var completed)
                        || !ReferenceEquals(completed, operation))
                        throw new InvalidOperationException(
                            "Bound-session deferred operation order changed while draining.");
                }
            }
        }
        finally
        {
            _drainGate.Release();
        }
    }
}
