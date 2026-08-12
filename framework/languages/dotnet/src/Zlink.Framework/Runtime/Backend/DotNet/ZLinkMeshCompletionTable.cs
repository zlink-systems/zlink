using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// Framework allocates the reply correlation and registers its waiter before it
// submits the request. The dispatch pump therefore only resolves known pending
// correlations; it never retains a reply that arrived before registration.
internal sealed class ZLinkMeshCompletionTable
{
    private const int DefaultCapacity = 4_096;

    internal delegate void CompletionHandler(
        MeshReceiveRecord record, IReadOnlyList<Message> parts);

    private readonly object _gate = new();
    private readonly Dictionary<MeshOperationId, PendingCompletion> _pending = new();
    private readonly int _capacity;
    private readonly ZLinkCompletionDispatcher _dispatcher;
    private TaskCompletionSource _drained = CompletedSignal();
    private int _outstandingOperations;
    private bool _closed;

    internal ZLinkMeshCompletionTable(
        int capacity = DefaultCapacity,
        ZLinkCompletionDispatcher? dispatcher = null)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
        _dispatcher = dispatcher ?? ZLinkCompletionDispatcher.Shared;
    }

    internal Task CompletionDrained
    {
        get
        {
            lock (_gate) return _drained.Task;
        }
    }

    public bool Register(MeshOperationId correlationId, CompletionHandler handler)
    {
        if (correlationId == default) return false;
        ArgumentNullException.ThrowIfNull(handler);
        lock (_gate)
        {
            if (_closed)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ShuttingDown,
                    "The mesh completion table is closed.");
            if (_pending.ContainsKey(correlationId))
                throw new InvalidOperationException(
                    "The reply correlation already has a waiter.");
            // A taken entry keeps its admission reservation until its callback
            // has run on the shared dispatcher. This transfers ownership to
            // the dispatch lane instead of creating a second unbounded queue.
            if (_outstandingOperations >= _capacity)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    $"The pending operation table reached its {_capacity:N0}-operation bound.");
            // The dispatch node is part of admission. All allocations happen
            // before the operation becomes visible in the pending table.
            var pending = new PendingCompletion(
                this,
                correlationId,
                handler);
            var nextDrained = _outstandingOperations == 0
                ? new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously)
                : null;
            _pending.Add(correlationId, pending);
            if (!_dispatcher.TryReserve(pending))
            {
                _pending.Remove(correlationId);
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    "The process-wide pending operation and completion callback "
                    + "bound of 4,096 was reached.");
            }
            if (nextDrained is not null)
                _drained = nextDrained;
            _outstandingOperations++;
        }
        return true;
    }

    public bool RegisterRequest(
        MeshOperationId correlationId,
        RequestCallback callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return Register(correlationId, (record, parts) =>
            callback(MapResult(record.TerminalResult, record.FailureErrno), parts));
    }

    // This is the only request submission entry point for the pull-dispatch
    // bridge. A synchronous rejection cannot produce a completion, so its
    // waiter is removed before the result is returned.
    public SubmitResult RegisterBeforeSubmit(
        MeshOperationId correlationId,
        CompletionHandler handler,
        Func<MeshOperationId, SubmitResult> submit)
    {
        ArgumentNullException.ThrowIfNull(handler);
        ArgumentNullException.ThrowIfNull(submit);
        if (!Register(correlationId, handler))
            throw new ArgumentException(
                "A non-default reply correlation is required.",
                nameof(correlationId));

        try
        {
            var result = submit(correlationId);
            if (result != SubmitResult.Ok)
                Unregister(correlationId);
            return result;
        }
        catch
        {
            Unregister(correlationId);
            throw;
        }
    }

    public SubmitResult RegisterRequestBeforeSubmit(
        MeshOperationId correlationId,
        RequestCallback callback,
        Func<MeshOperationId, SubmitResult> submit)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return RegisterBeforeSubmit(
            correlationId,
            (record, parts) => callback(
                MapResult(record.TerminalResult, record.FailureErrno),
                parts),
            submit);
    }

    private bool Unregister(MeshOperationId correlationId)
    {
        PendingCompletion? pending;
        lock (_gate)
            _pending.Remove(correlationId, out pending);
        if (pending is null)
            return false;
        ReleaseReservation(pending, null);
        return true;
    }

    // Cancellation competes with reply and shutdown for the same table entry.
    // Only the path that removes the entry may publish its terminal result.
    internal bool TryCancel(MeshOperationId correlationId) =>
        Unregister(correlationId);

    internal CancellationTokenRegistration RegisterCancellation(
        MeshOperationId correlationId,
        CancellationToken cancellationToken,
        Action completeCancellation)
    {
        ArgumentNullException.ThrowIfNull(completeCancellation);
        PendingCompletion? pending;
        lock (_gate)
        {
            if (!_pending.TryGetValue(correlationId, out pending))
                return default;
            pending.SetCancellationAction(completeCancellation);
        }
        return cancellationToken.Register(
            static state => ((PendingCompletion)state!).CancelFromToken(),
            pending);
    }

    public void Complete(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts)
    {
        if (!TryTake(record.OperationId, out var pending))
        {
            // A terminal after cancellation, timeout, or shutdown no longer has
            // an owner. It must not be attached to a later request.
            ZLinkMessageParts.DisposeAll(parts);
            return;
        }
        // Ownership of parts transfers to the registered handler. Once the
        // callback starts, the table cannot safely dispose them on exception:
        // the handler may already have transferred or disposed the messages.
        pending.PrepareReply(record, parts);
        _dispatcher.Post(pending);
    }

    public void FailAll(RequestResult result)
    {
        lock (_gate)
        {
            _closed = true;
            foreach (var entry in _pending)
            {
                entry.Value.PrepareFailure(result);
                _dispatcher.Post(entry.Value);
            }
            _pending.Clear();
        }
    }

    private bool TryTake(
        MeshOperationId correlationId,
        out PendingCompletion pending)
    {
        lock (_gate)
            return _pending.Remove(correlationId, out pending!);
    }

    private void TryDispatchCancellation(PendingCompletion expected)
    {
        lock (_gate)
        {
            if (!_pending.TryGetValue(expected.CorrelationId, out var current)
                || !ReferenceEquals(current, expected))
                return;
            _pending.Remove(expected.CorrelationId);
        }
        expected.PrepareCancellation();
        _dispatcher.Post(expected);
    }

    private void ReleaseReservation(
        PendingCompletion pending,
        Exception? failure)
    {
        TaskCompletionSource? drained = null;
        lock (_gate)
        {
            if (_outstandingOperations <= 0)
                return;
            _outstandingOperations--;
            _dispatcher.ReleaseReservation(pending);
            if (_outstandingOperations == 0)
                drained = _drained;
        }
        drained?.TrySetResult();
        if (failure is not null)
        {
            try
            {
                ZLinkFrameworkDebugLog.TaskFailure(
                    "mesh-completion-callback",
                    failure);
            }
            catch
            {
                // Diagnostics cannot keep the table's drain contract pending.
            }
        }
    }

    private static TaskCompletionSource CompletedSignal()
    {
        var signal = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        signal.TrySetResult();
        return signal;
    }

    private sealed class PendingCompletion : ZLinkCompletionDispatcher.WorkItem
    {
        private static readonly IReadOnlyList<Message> EmptyParts =
            Array.Empty<Message>();

        private readonly ZLinkMeshCompletionTable _owner;
        private readonly CompletionHandler _handler;
        private MeshReceiveRecord _record;
        private IReadOnlyList<Message>? _parts;
        private Action? _cancellationAction;
        private DispatchKind _kind;

        internal PendingCompletion(
            ZLinkMeshCompletionTable owner,
            MeshOperationId correlationId,
            CompletionHandler handler)
        {
            _owner = owner;
            CorrelationId = correlationId;
            _handler = handler;
        }

        internal MeshOperationId CorrelationId { get; }

        internal void SetCancellationAction(Action action)
        {
            if (_cancellationAction is not null)
                throw new InvalidOperationException(
                    "A cancellation callback is already registered for this operation.");
            _cancellationAction = action;
        }

        internal void CancelFromToken() =>
            _owner.TryDispatchCancellation(this);

        internal void PrepareReply(
            MeshReceiveRecord record,
            IReadOnlyList<Message> parts)
        {
            _record = record;
            _parts = parts;
            _kind = DispatchKind.Reply;
        }

        internal void PrepareFailure(RequestResult result)
        {
            _record = MeshReceiveRecord.CompletionFailure(
                CorrelationId,
                result);
            _parts = EmptyParts;
            _kind = DispatchKind.Failure;
        }

        internal void PrepareCancellation()
        {
            _kind = DispatchKind.Cancellation;
        }

        internal override void Execute()
        {
            switch (_kind)
            {
                case DispatchKind.Reply:
                case DispatchKind.Failure:
                    _handler(_record, _parts!);
                    break;
                case DispatchKind.Cancellation:
                    _cancellationAction!();
                    break;
                default:
                    throw new InvalidOperationException(
                        "Completion work was dispatched without a terminal result.");
            }
        }

        internal override void Completed(Exception? failure)
        {
            _parts = null;
            _cancellationAction = null;
            _owner.ReleaseReservation(this, failure);
        }

        private enum DispatchKind
        {
            None = 0,
            Reply = 1,
            Failure = 2,
            Cancellation = 3
        }
    }

    public static RequestResult MapResult(int terminalResult, int failureErrno)
    {
        _ = failureErrno;
        if (terminalResult == 0) return RequestResult.Ok;
        var result = (RequestResult)terminalResult;
        return Enum.IsDefined(result) ? result : RequestResult.InternalError;
    }
}
