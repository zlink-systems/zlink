using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkBoundSessionDispatchScope : IAsyncDisposable
{
    private const int MaxDeferredOperations = 4096;
    private static readonly AsyncLocal<ZLinkBoundSessionDispatchScope?> CurrentScope = new();
    private readonly string _actorId;
    private readonly ZLinkStateLane _lane = new();
    private readonly Queue<Func<CancellationToken, ValueTask>> _deferredOperations = new();
    // This serializes the external deferred-operation protocol. It is not state ownership:
    // the queue and drained flag remain owned by _lane.
    private readonly SemaphoreSlim _drainGate = new(1, 1);
    private readonly ZLinkBoundSessionDispatchScope? _previous;
    private bool _drained;
    private int _disposed;

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
            Volatile.Write(ref _disposed, 1);
            await _lane.DisposeAsync().ConfigureAwait(false);
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
        if (Volatile.Read(ref scope._disposed) != 0)
            return false;

        try
        {
            return AwaitStateLane(scope._lane.RunAsync(() => scope.Defer(operationAsync)));
        }
        catch (ObjectDisposedException) when (Volatile.Read(ref scope._disposed) != 0)
        {
            return false;
        }
    }

    public async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        await _drainGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            while (true)
            {
                var operation = await _lane.RunAsync(PrepareDrain).ConfigureAwait(false);
                if (operation is null) return;

                await operation(cancellationToken).ConfigureAwait(false);
                await _lane.RunAsync(() => CompleteDrain(operation)).ConfigureAwait(false);
            }
        }
        finally
        {
            _drainGate.Release();
        }
    }

    private bool Defer(Func<CancellationToken, ValueTask> operationAsync)
    {
        if (_drained) return false;
        if (_deferredOperations.Count >= MaxDeferredOperations)
            throw new InvalidOperationException("Bound-session deferred submit queue is full.");
        _deferredOperations.Enqueue(operationAsync);
        return true;
    }

    private Func<CancellationToken, ValueTask>? PrepareDrain()
    {
        if (_drained) return null;
        if (_deferredOperations.TryPeek(out var operation)) return operation;
        _drained = true;
        return null;
    }

    private void CompleteDrain(Func<CancellationToken, ValueTask> operation)
    {
        if (!_deferredOperations.TryDequeue(out var completed)
            || !ReferenceEquals(completed, operation))
            throw new InvalidOperationException(
                "Bound-session deferred operation order changed while draining.");
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
