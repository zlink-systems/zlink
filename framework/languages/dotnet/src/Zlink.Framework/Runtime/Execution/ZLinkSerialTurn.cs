namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkSerialTurn
{
    private static readonly AsyncLocal<ZLinkSerialTurn?> CurrentTurn = new();

    private readonly Func<ZLinkSerialTurn, Task> _postResume;
    private readonly Func<Func<CancellationToken, ValueTask>, bool> _postCallback;
    private readonly Action<Exception> _reportError;
    internal ZLinkSerialWorkItem Item { get; }
    internal ZLinkSerialExecutionQueue? ExecutionGate { get; }

    internal ZLinkSerialWorkItem? LifecycleOwner =>
        Item.LifecycleOwner ?? (Item.Lane == ZLinkSerialWorkLane.Lifecycle ? Item : null);
    private Task? _ownerTask;

    private TaskCompletionSource _suspended = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );

    private int _suspendSignaled;
    private int _yieldUsed;

    public ZLinkSerialTurn(
        Func<ZLinkSerialTurn, Task> postResume,
        Func<Func<CancellationToken, ValueTask>, bool> postCallback,
        Action<Exception> reportError,
        ZLinkSerialWorkItem item,
        ZLinkSerialExecutionQueue? executionGate = null
    )
    {
        _postResume = postResume;
        _postCallback = postCallback;
        _reportError = reportError;
        Item = item;
        ExecutionGate = executionGate;
    }

    public static ZLinkSerialTurn? Current => CurrentTurn.Value;

    public Task Suspended => Volatile.Read(ref _suspended).Task;

    public Task? OwnerTask => Volatile.Read(ref _ownerTask);

    public void BindOwnerTask(Task ownerTask)
    {
        Volatile.Write(ref _ownerTask, ownerTask);
    }

    public void ResetSuspension()
    {
        Volatile.Write(ref _yieldUsed, 0);
        Volatile.Write(
            ref _suspended,
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
        );
        Volatile.Write(ref _suspendSignaled, 0);
    }

    public bool TryPost(Func<CancellationToken, ValueTask> callback)
    {
        if (!Item.IsExecutionAllowed)
            return false;
        using var scope = Push(this);
        return _postCallback(callback);
    }

    public void ReportError(Exception exception)
    {
        _reportError(exception);
    }

    public static Scope Push(ZLinkSerialTurn turn)
    {
        var previous = CurrentTurn.Value;
        CurrentTurn.Value = turn;
        return new Scope(previous);
    }

    public static Scope Suppress()
    {
        var previous = CurrentTurn.Value;
        CurrentTurn.Value = null;
        return new Scope(previous);
    }

    internal ZLinkSerialTurn AdmitYield(ZLinkSerialTurn? capturedTurn, string operation)
    {
        if (
            ZLinkApplicationExecutionContext.Current is not { YieldAllowed: true }
            || !ReferenceEquals(Current, this)
            || !ReferenceEquals(capturedTurn, this)
            || Volatile.Read(ref _suspendSignaled) != 0
            || Item.Completion.IsCompleted
            || Interlocked.CompareExchange(ref _yieldUsed, 1, 0) != 0
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"{operation} Yield requires the current SpotWide application turn and may be used only once per turn."
            );
        return this;
    }

    public async ValueTask<T> YieldFrameworkCallAsync<T>(
        Func<CancellationToken, ValueTask<T>> submit,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var operation = submit(cancellationToken);
        if (operation.IsCompletedSuccessfully)
            return operation.Result;

        SignalSuspended();
        try
        {
            return await operation.AsTask().WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            await _postResume(this).ConfigureAwait(false);
        }
    }

    public async ValueTask YieldFrameworkCallAsync(
        Func<CancellationToken, ValueTask> submit,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var operation = submit(cancellationToken);
        if (operation.IsCompletedSuccessfully)
            return;

        SignalSuspended();
        try
        {
            await operation.AsTask().WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            await _postResume(this).ConfigureAwait(false);
        }
    }

    internal void SignalSuspended()
    {
        if (Interlocked.Exchange(ref _suspendSignaled, 1) == 0)
            Volatile.Read(ref _suspended).TrySetResult();
    }

    public readonly struct Scope : IDisposable
    {
        private readonly ZLinkSerialTurn? _previous;

        internal Scope(ZLinkSerialTurn? previous)
        {
            _previous = previous;
        }

        public void Dispose()
        {
            CurrentTurn.Value = _previous;
        }
    }
}
