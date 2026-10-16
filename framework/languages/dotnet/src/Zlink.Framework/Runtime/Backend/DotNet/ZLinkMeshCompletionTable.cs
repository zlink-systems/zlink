using System.Threading.Tasks.Sources;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// Framework allocates the reply correlation and registers its waiter before it
// submits the request. The dispatch pump therefore only resolves known pending
// correlations; it never retains a reply that arrived before registration.
internal sealed class ZLinkMeshCompletionTable
{
    internal delegate void CompletionHandler(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts
    );

    private readonly object _gate = new();
    private readonly Dictionary<MeshOperationId, PendingCompletion> _pending = new();
    private TaskCompletionSource? _drained;
    private int _outstandingOperations;
    private bool _closed;

    internal Task CompletionDrained
    {
        get
        {
            lock (_gate)
                return _outstandingOperations == 0
                    ? Task.CompletedTask
                    : (
                        _drained ??= new TaskCompletionSource(
                            TaskCreationOptions.RunContinuationsAsynchronously
                        )
                    ).Task;
        }
    }

    public bool Register(MeshOperationId correlationId, CompletionHandler handler)
    {
        if (correlationId == default)
            return false;
        ArgumentNullException.ThrowIfNull(handler);
        Add(new PendingReply(this, correlationId, handler));
        return true;
    }

    internal PendingSend RegisterSend(MeshOperationId operationId)
    {
        if (operationId == default)
            throw new ArgumentException(
                "A non-default operation is required.",
                nameof(operationId)
            );
        var pending = new PendingSend(this, operationId);
        Add(pending);
        return pending;
    }

    private void Add(PendingCompletion pending)
    {
        lock (_gate)
        {
            if (_closed)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ShuttingDown,
                    "The mesh completion table is closed."
                );
            // The dispatcher node exists before the operation becomes visible
            // in the pending table, preserving registration-before-submit.
            if (!_pending.TryAdd(pending.CorrelationId, pending))
                throw new InvalidOperationException("The reply correlation already has a waiter.");
            _outstandingOperations++;
        }
    }

    public bool RegisterRequest(MeshOperationId correlationId, ZLinkBackendRequestCallback callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return Register(
            correlationId,
            (record, parts) =>
                callback(MapResult(record.TerminalResult, record.FailureErrno), parts)
        );
    }

    // This is the only request submission entry point for the pull-dispatch
    // bridge. A synchronous rejection cannot produce a completion, so its
    // waiter is removed before the result is returned.
    public SubmitResult RegisterBeforeSubmit(
        MeshOperationId correlationId,
        CompletionHandler handler,
        Func<MeshOperationId, SubmitResult> submit
    )
    {
        ArgumentNullException.ThrowIfNull(handler);
        ArgumentNullException.ThrowIfNull(submit);
        if (!Register(correlationId, handler))
            throw new ArgumentException(
                "A non-default reply correlation is required.",
                nameof(correlationId)
            );

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
        ZLinkBackendRequestCallback callback,
        Func<MeshOperationId, SubmitResult> submit
    )
    {
        ArgumentNullException.ThrowIfNull(callback);
        return RegisterBeforeSubmit(
            correlationId,
            (record, parts) =>
                callback(MapResult(record.TerminalResult, record.FailureErrno), parts),
            submit
        );
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
    internal bool TryCancel(MeshOperationId correlationId) => Unregister(correlationId);

    internal CancellationTokenRegistration RegisterCancellation(
        MeshOperationId correlationId,
        CancellationToken cancellationToken,
        Action completeCancellation
    )
    {
        ArgumentNullException.ThrowIfNull(completeCancellation);
        PendingReply? pending;
        lock (_gate)
        {
            if (!_pending.TryGetValue(correlationId, out var entry))
                return default;
            pending = (PendingReply)entry;
            pending.SetCancellationAction(completeCancellation);
        }
        return cancellationToken.Register(
            static state => ((PendingReply)state!).CancelFromToken(),
            pending
        );
    }

    public void Complete(MeshReceiveRecord record, IReadOnlyList<Message> parts)
    {
        if (!TryComplete(record, parts))
            ZLinkMessageParts.DisposeAll(parts);
    }

    // A native node may also serve raw pull callers. Ownership transfers only
    // when this table actually owns the operation, not merely because a sink
    // was installed by a Framework wrapper.
    internal bool TryComplete(MeshReceiveRecord record, IReadOnlyList<Message> parts)
    {
        if (!TryTake(record.OperationId, out var pending))
            return false;
        ((PendingReply)pending).PrepareReply(record, parts);
        ZLinkCompletionDispatcher.Post(pending);
        return true;
    }

    private void CompleteSend(MeshOperationId operationId, Exception? failure)
    {
        if (!TryTake(operationId, out var pending))
            return;
        ((PendingSend)pending).PrepareSend(failure);
        ZLinkCompletionDispatcher.Post(pending);
    }

    public void FailAll(RequestResult result)
    {
        PendingCompletion[] entries;
        lock (_gate)
        {
            _closed = true;
            entries = _pending.Values.ToArray();
            _pending.Clear();
        }
        foreach (var pending in entries)
        {
            pending.PrepareFailure(result);
            ZLinkCompletionDispatcher.Post(pending);
        }
    }

    private bool TryTake(MeshOperationId correlationId, out PendingCompletion pending)
    {
        lock (_gate)
            return _pending.Remove(correlationId, out pending!);
    }

    private void TryDispatchCancellation(PendingReply expected)
    {
        lock (_gate)
        {
            if (
                !_pending.TryGetValue(expected.CorrelationId, out var current)
                || !ReferenceEquals(current, expected)
            )
                return;
            _pending.Remove(expected.CorrelationId);
        }
        expected.PrepareCancellation();
        ZLinkCompletionDispatcher.Post(expected);
    }

    private void ReleaseReservation(PendingCompletion pending, Exception? failure)
    {
        TaskCompletionSource? drained = null;
        lock (_gate)
        {
            if (_outstandingOperations <= 0)
                return;
            _outstandingOperations--;
            if (_outstandingOperations == 0)
            {
                drained = _drained;
                _drained = null;
            }
        }
        drained?.TrySetResult();
        if (failure is not null)
        {
            try
            {
                ZLinkFrameworkDebugLog.TaskFailure("mesh-completion-callback", failure);
            }
            catch
            {
                // Diagnostics cannot keep the table's drain contract pending.
            }
        }
    }

    internal abstract class PendingCompletion : ZLinkCompletionDispatcher.WorkItem
    {
        protected readonly ZLinkMeshCompletionTable Owner;
        internal MeshOperationId CorrelationId { get; }

        protected PendingCompletion(ZLinkMeshCompletionTable owner, MeshOperationId correlationId)
        {
            Owner = owner;
            CorrelationId = correlationId;
        }

        internal abstract void PrepareFailure(RequestResult result);

        internal override void Completed(Exception? failure) =>
            Owner.ReleaseReservation(this, failure);
    }

    internal sealed class PendingSend : PendingCompletion, IValueTaskSource
    {
        private ManualResetValueTaskSourceCore<bool> _source;
        private Task? _admission;
        private Exception? _failure;

        internal PendingSend(ZLinkMeshCompletionTable owner, MeshOperationId operationId)
            : base(owner, operationId) { }

        internal ValueTask Completion => new(this, _source.Version);

        // Called by the submitter before it returns Completion for an admission
        // the binding decides later (spec 01 §10); it completes in a dispatcher turn.
        internal void Observe(Task admission)
        {
            _admission = admission;
            admission.ConfigureAwait(false).GetAwaiter().UnsafeOnCompleted(CompleteAdmission);
        }

        private void CompleteAdmission()
        {
            var admission = _admission!;
            _admission = null;
            Owner.CompleteSend(CorrelationId, AdmissionFailure(admission));
        }

        private static Exception? AdmissionFailure(Task admission)
        {
            try
            {
                admission.GetAwaiter().GetResult();
                return null;
            }
            catch (Exception error)
            {
                return error;
            }
        }

        internal void PrepareSend(Exception? failure) => _failure = failure;

        internal override void PrepareFailure(RequestResult result) =>
            _failure = ZLinkRequestFailureMapper.CreateCompletionException(result, "MeshNode send");

        internal override void Execute()
        {
            if (_failure is { } failure)
                _source.SetException(failure);
            else
                _source.SetResult(true);
        }

        void IValueTaskSource.GetResult(short token) => _source.GetResult(token);

        ValueTaskSourceStatus IValueTaskSource.GetStatus(short token) => _source.GetStatus(token);

        void IValueTaskSource.OnCompleted(
            Action<object?> continuation,
            object? state,
            short token,
            ValueTaskSourceOnCompletedFlags flags
        ) => _source.OnCompleted(continuation, state, token, flags);
    }

    private sealed class PendingReply : PendingCompletion
    {
        private static readonly IReadOnlyList<Message> EmptyParts = Array.Empty<Message>();

        private readonly CompletionHandler _handler;
        private MeshReceiveRecord _record;
        private IReadOnlyList<Message>? _parts;
        private Action? _cancellationAction;
        private DispatchKind _kind;

        internal PendingReply(
            ZLinkMeshCompletionTable owner,
            MeshOperationId correlationId,
            CompletionHandler handler
        )
            : base(owner, correlationId)
        {
            _handler = handler;
        }

        internal void SetCancellationAction(Action action)
        {
            if (_cancellationAction is not null)
                throw new InvalidOperationException(
                    "A cancellation callback is already registered for this operation."
                );
            _cancellationAction = action;
        }

        internal void CancelFromToken() => Owner.TryDispatchCancellation(this);

        internal void PrepareReply(MeshReceiveRecord record, IReadOnlyList<Message> parts)
        {
            _record = record;
            _parts = parts;
            _kind = DispatchKind.Reply;
        }

        internal override void PrepareFailure(RequestResult result)
        {
            _record = MeshReceiveRecord.CompletionFailure(CorrelationId, result);
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
                        "Completion work was dispatched without a terminal result."
                    );
            }
        }

        internal override void Completed(Exception? failure)
        {
            _parts = null;
            _cancellationAction = null;
            base.Completed(failure);
        }

        private enum DispatchKind
        {
            None = 0,
            Reply = 1,
            Failure = 2,
            Cancellation = 3,
        }
    }

    public static RequestResult MapResult(int terminalResult, int failureErrno)
    {
        _ = failureErrno;
        if (terminalResult == 0)
            return RequestResult.Ok;
        var result = (RequestResult)terminalResult;
        return Enum.IsDefined(result) ? result : RequestResult.InternalError;
    }
}
