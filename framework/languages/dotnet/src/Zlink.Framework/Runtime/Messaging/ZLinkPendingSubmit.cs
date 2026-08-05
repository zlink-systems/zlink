namespace Zlink.Framework.Runtime.Messaging;

internal sealed class PendingSubmit : IDisposable
{
    private readonly IPendingSubmitCompletion _completion;
    private readonly object _admissionGate;
    private readonly Action<PendingSubmit> _wake;
    private CancellationTokenRegistration _callerCancellationRegistration;
    private int _completed;
    private Timer? _deadlineTimer;
    private Exception? _lastSubmitFailure;
    private CancellationTokenRegistration _stopCancellationRegistration;

    private PendingSubmit(
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, bool> trySubmit,
        DateTimeOffset? deadline,
        object admissionGate,
        Action<PendingSubmit> wake,
        IPendingSubmitCompletion completion,
        bool completeOnAccepted,
        string operationId)
    {
        Parts = parts;
        TrySubmit = trySubmit;
        Deadline = deadline;
        _admissionGate = admissionGate;
        _wake = wake;
        _completion = completion;
        CompleteOnAccepted = completeOnAccepted;
        OperationId = operationId;
    }

    public IReadOnlyList<Message> Parts { get; }

    public Func<IReadOnlyList<Message>, bool> TrySubmit { get; }

    public DateTimeOffset? Deadline { get; }

    public bool CompleteOnAccepted { get; }

    public string OperationId { get; }

    public Task Task => _completion.Task;

    public bool IsCompleted => Volatile.Read(ref _completed) != 0 || _completion.Task.IsCompleted;

    public void Dispose()
    {
        _deadlineTimer?.Dispose();
        _callerCancellationRegistration.Dispose();
        _stopCancellationRegistration.Dispose();
        foreach (var part in Parts) part.Dispose();
    }

    public static PendingSubmit CreateCommand(
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, bool> trySubmit,
        DateTimeOffset? deadline,
        object admissionGate,
        Action<PendingSubmit> wake,
        string operationId)
    {
        return new PendingSubmit(
            parts,
            trySubmit,
            deadline,
            admissionGate,
            wake,
            new ObjectPendingSubmitCompletion(
                new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously)),
            true,
            operationId);
    }

    public static PendingSubmit CreateRequest<T>(
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, bool> trySubmit,
        DateTimeOffset? deadline,
        object admissionGate,
        Action<PendingSubmit> wake,
        ZLinkRequestCompletion<T> completion,
        string operationId)
    {
        return new PendingSubmit(
            parts,
            trySubmit,
            deadline,
            admissionGate,
            wake,
            new RequestPendingSubmitCompletion<T>(completion),
            false,
            operationId);
    }

    public void Activate(CancellationToken cancellationToken, CancellationToken stopToken)
    {
        StartDeadlineTimer();
        RegisterCancellation(cancellationToken, stopToken);
    }

    public void TryComplete(object? result)
    {
        lock (_admissionGate)
        {
            if (Interlocked.Exchange(ref _completed, 1) == 0) _completion.TrySetResult(result);
        }
    }

    public void TryCancel(CancellationToken cancellationToken)
    {
        var completed = false;
        lock (_admissionGate)
        {
            if (Interlocked.Exchange(ref _completed, 1) == 0)
            {
                _completion.TrySetCanceled(cancellationToken);
                completed = true;
            }
        }

        if (completed) _wake(this);
    }

    public void TryFail(Exception exception)
    {
        var completed = false;
        lock (_admissionGate)
        {
            if (Interlocked.Exchange(ref _completed, 1) == 0)
            {
                _completion.TrySetException(exception);
                completed = true;
            }
        }


        if (completed) _wake(this);
    }

    public void RecordSubmitFailure(Exception exception)
    {
        Volatile.Write(ref _lastSubmitFailure, exception);
    }

    private void StartDeadlineTimer()
    {
        if (Deadline is not { } deadline) return;

        var dueTime = deadline - DateTimeOffset.UtcNow;
        if (dueTime < TimeSpan.Zero) dueTime = TimeSpan.Zero;

        _deadlineTimer = new Timer(static state =>
        {
            var item = (PendingSubmit)state!;
            item.TryFail(item.CreateDeadlineException());
        }, this, dueTime, Timeout.InfiniteTimeSpan);
    }

    private Exception CreateDeadlineException()
    {
        var lastSubmitFailure = Volatile.Read(ref _lastSubmitFailure);
        return CompleteOnAccepted
            ? CreateCommandDeadlineException(lastSubmitFailure)
            : ZLinkRequestFailureMapper.CreateSubmitTimeoutException(
                lastSubmitFailure,
                "ZLink request submit");
    }

    private static Exception CreateCommandDeadlineException(Exception? lastSubmitFailure)
    {
        return ZLinkRequestFailureMapper.CreateSubmitTimeoutException(
            lastSubmitFailure,
            "ZLink async submit");
    }

    private void RegisterCancellation(CancellationToken cancellationToken, CancellationToken stopToken)
    {
        _callerCancellationRegistration = RegisterCancellation(cancellationToken);
        _stopCancellationRegistration = RegisterStop(stopToken);
    }

    private CancellationTokenRegistration RegisterStop(CancellationToken token)
    {
        if (token.IsCancellationRequested)
        {
            TryFail(new ZLinkSubmitShutdownException());
            return default;
        }

        return token.Register(static state =>
        {
            ((PendingSubmit)state!).TryFail(new ZLinkSubmitShutdownException());
        }, this);
    }

    private CancellationTokenRegistration RegisterCancellation(CancellationToken token)
    {
        if (token.IsCancellationRequested)
        {
            TryCancel(token);
            return default;
        }

        return token.Register(static state =>
        {
            var cancellation = (CancellationState)state!;
            cancellation.Submit.TryCancel(cancellation.Token);
        }, new CancellationState(this, token));
    }

    private interface IPendingSubmitCompletion
    {
        Task Task { get; }

        void TrySetResult(object? result);

        void TrySetCanceled(CancellationToken cancellationToken);

        void TrySetException(Exception exception);
    }

    private sealed class ObjectPendingSubmitCompletion(TaskCompletionSource<object?> source)
        : IPendingSubmitCompletion
    {
        public Task Task => source.Task;

        public void TrySetResult(object? result)
        {
            source.TrySetResult(result);
        }

        public void TrySetCanceled(CancellationToken cancellationToken)
        {
            source.TrySetCanceled(cancellationToken);
        }

        public void TrySetException(Exception exception)
        {
            source.TrySetException(exception);
        }
    }

    private sealed class RequestPendingSubmitCompletion<T>(ZLinkRequestCompletion<T> completion)
        : IPendingSubmitCompletion
    {
        public Task Task => completion.Task;

        public void TrySetResult(object? result)
        {
            throw new InvalidOperationException("Request submissions complete only from their native callback.");
        }

        public void TrySetCanceled(CancellationToken cancellationToken)
        {
            completion.Cancel(cancellationToken);
        }

        public void TrySetException(Exception exception)
        {
            completion.Fail(exception);
        }
    }

    private sealed record CancellationState(PendingSubmit Submit, CancellationToken Token);
}

internal sealed class ZLinkPendingAdmissionFullException : Exception
{
    public ZLinkPendingAdmissionFullException()
        : base("ZLink async submit pending admission capacity is full.")
    {
    }
}

internal sealed class ZLinkSubmitShutdownException : Exception
{
    public ZLinkSubmitShutdownException()
        : base("ZLink async submit admission is closed.")
    {
    }
}
