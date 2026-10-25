using Zlink.Framework.Runtime.Execution;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Actors;

// Actor lifecycle policy over the runtime's common serial execution engine.
// Ordinary dispatch uses the application lane. A deferred Join barrier uses
// the lifecycle lane so it runs before already queued ordinary work, while a
// terminal barrier closes admission and joins the application lane after all
// work accepted before the close.
internal sealed class ZLinkActorSerialExecutor : IAsyncDisposable
{
    private readonly ZLinkStateLane _lane = new();
    private readonly ZLinkSerialExecutionQueue _queue;
    private bool _admissionClosed;
    private int _acceptedWaiters;
    private int _pendingRequests;

    public ZLinkActorSerialExecutor(ZLinkExecutionLanePolicy? lanePolicy = null)
    {
        var reporter = MailboxFailureReporter.Instance;
        _queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(reporter, CancellationToken.None, this),
            reporter,
            CancellationToken.None,
            lanePolicy ?? ZLinkExecutionLanePolicy.Default
        );
    }

    public async ValueTask DisposeAsync()
    {
        await _queue.DisposeAsync().ConfigureAwait(false);
        await _lane.DisposeAsync().ConfigureAwait(false);
    }

    internal bool IsDisposed => _queue.IsDisposed;

    internal ValueTask ReleaseExecutionSourceAsync() => _queue.ReleaseExecutionSourceAsync();

    internal void BindExecutionOwner(Func<ZLinkSerialExecutionQueue?> provider) =>
        _queue.BindExecutionOwner(provider);

    public int PendingRequestCount
    {
        get => AwaitStateLane(_lane.RunAsync(() => _pendingRequests));
    }

    internal int PendingLifecycleCount => _queue.LifecyclePendingCount;

    internal void NotifyReadiness() => _queue.NotifyReadiness();

    internal IReadOnlyList<Task> AdmitDurablePrefix(IReadOnlyList<ZLinkActorDurableRecord> records)
    {
        return AwaitStateLane(
            _lane.RunAsync(() =>
            {
                var terminals = new List<Task>(records.Count);
                foreach (var record in records)
                {
                    ZLinkSerialPostAdmission admission;
                    ZLinkSerialWorkItem item;
                    using (ExecutionContext.SuppressFlow())
                        admission = _queue.TryPostDurableApplication(
                            record.Operation,
                            record.PrepareAsync ?? (static _ => ValueTask.CompletedTask),
                            record.CancelPreparation ?? (static () => { }),
                            out item,
                            ready: record.Ready,
                            errorReporter: record.ReportFailure
                        );
                    if (admission != ZLinkSerialPostAdmission.Accepted)
                        throw new InvalidOperationException("Actor FIFO is closed.");
                    record.OnAdmitted?.Invoke();
                    record.OnWithdrawable?.Invoke(() => _queue.WithdrawAsync(item));
                    terminals.Add(item.Completion);
                }
                return (IReadOnlyList<Task>)terminals;
            })
        );
    }

    public ValueTask<Turn> EnterAsync(
        CancellationToken cancellationToken,
        bool countAsPendingRequest = false
    ) => EnterAsync(cancellationToken, countAsPendingRequest, ZLinkSerialWorkLane.Application);

    public ValueTask<Turn> EnterLifecycleAsync(CancellationToken cancellationToken) =>
        EnterAsync(cancellationToken, false, ZLinkSerialWorkLane.Lifecycle);

    private ValueTask<Turn> EnterAsync(
        CancellationToken cancellationToken,
        bool countAsPendingRequest,
        ZLinkSerialWorkLane lane
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var waiter = AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_admissionClosed)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        "Actor dispatch admission is closed for a terminal lifecycle transition."
                    );
                return PostWaiterOnLane(lane, cancellationToken, countAsPendingRequest);
            })
        );
        return AwaitTurnAsync(waiter);
    }

    public BarrierReservation ReserveBarrier()
    {
        var cancellation = new CancellationTokenSource();
        var waiter = AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_admissionClosed)
                {
                    cancellation.Dispose();
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "Actor lifecycle admission is closed for a terminal transition."
                    );
                }
                return PostWaiterOnLane(
                    ZLinkSerialWorkLane.Lifecycle,
                    cancellation.Token,
                    countAsPendingRequest: false
                );
            })
        );
        return new BarrierReservation(waiter, cancellation);
    }

    public BarrierReservation CloseAdmissionAndReserveLifecycleBarrier()
    {
        var cancellation = new CancellationTokenSource();
        var waiter = AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_admissionClosed)
                {
                    cancellation.Dispose();
                    throw new InvalidOperationException(
                        "Actor terminal lifecycle barrier was already reserved."
                    );
                }

                _admissionClosed = true;
                try
                {
                    // Terminal cleanup must remain behind every ordinary turn that
                    // was accepted before admission closed.
                    return PostWaiterOnLane(
                        ZLinkSerialWorkLane.Application,
                        cancellation.Token,
                        countAsPendingRequest: false
                    );
                }
                catch
                {
                    _admissionClosed = false;
                    cancellation.Dispose();
                    throw;
                }
            })
        );
        return new BarrierReservation(waiter, cancellation);
    }

    /// <summary>
    /// Reopens admission for an Actor arriving back on this node. A completed
    /// migration leaves this mailbox closed, and an incoming handoff has to
    /// enter it before anything can reopen it, so a returning Actor would meet
    /// the state it left. Only an idle mailbox reopens: one still holding a
    /// turn or queued waiters is mid-lifecycle and keeps its closed admission.
    /// </summary>
    public bool TryReopenAdmissionForIncomingHandoff()
    {
        return AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (!_admissionClosed)
                    return true;
                if (_acceptedWaiters != 0 || _queue.HasPendingDurableApplication)
                    return false;
                _admissionClosed = false;
                return true;
            })
        );
    }

    public void ReopenAdmission()
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_acceptedWaiters != 0 || _queue.HasPendingDurableApplication)
                    throw new InvalidOperationException(
                        "Actor dispatch admission cannot reopen while a previous lifecycle turn remains."
                    );
                _admissionClosed = false;
            })
        );
    }

    private Waiter PostWaiterOnLane(
        ZLinkSerialWorkLane lane,
        CancellationToken cancellationToken,
        bool countAsPendingRequest
    )
    {
        var waiter = new Waiter(this, cancellationToken, countAsPendingRequest);
        if (countAsPendingRequest)
            _pendingRequests++;
        // Queue draining is a long-running task. It starts after this method returns, so
        // SuppressFlow is sufficient: TryPost only admits and schedules a cold task; it does
        // not synchronously invoke waiter.RunAsync.
        ZLinkSerialPostAdmission admission;
        ZLinkSerialWorkItem item;
        using (ExecutionContext.SuppressFlow())
            admission =
                lane == ZLinkSerialWorkLane.Lifecycle
                    ? _queue.TryPostNextWithAdmission(waiter.RunAsync, out item)
                    : _queue.TryPostApplicationWithAdmission(waiter.RunAsync, out item);
        if (admission == ZLinkSerialPostAdmission.Accepted)
        {
            waiter.MarkAccepted();
            _acceptedWaiters++;
            item.AddTerminalRelease(() => OnWaiterFinished(waiter));
            waiter.BindItem(item);
            if (item.Preparation is not null)
            {
                _ = item.Completion.ContinueWith(
                    static (task, state) =>
                        ((Waiter)state!).FailPreparation(task.Exception!.InnerException!),
                    waiter,
                    CancellationToken.None,
                    TaskContinuationOptions.OnlyOnFaulted
                        | TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default
                );
                ZLinkApplicationJobQueueInvocation.CurrentScope?.ReleaseDurablePermit();
            }
            return waiter;
        }

        if (countAsPendingRequest)
            _pendingRequests--;
        waiter.Dispose();
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ShuttingDown,
            "Actor dispatch queue is closed."
        );
    }

    private ValueTask OnWaiterStartedAsync(Waiter waiter) =>
        _lane.RunAsync(() =>
        {
            if (waiter.TryStartAccepted() && waiter.CountsAsPendingRequest)
                _pendingRequests--;
        });

    private void OnWaiterFinished(Waiter waiter)
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                var previous = waiter.FinishAccepted();
                if (previous == Waiter.NotAccepted)
                    return;
                if (previous == Waiter.AcceptedWaiting && waiter.CountsAsPendingRequest)
                    _pendingRequests--;
                _acceptedWaiters--;
            })
        );
    }

    private static async ValueTask<Turn> AwaitTurnAsync(Waiter waiter)
    {
        try
        {
            return await waiter.Task.ConfigureAwait(false);
        }
        finally
        {
            waiter.DisposeCancellationRegistration();
        }
    }

    public sealed class BarrierReservation(Waiter waiter, CancellationTokenSource cancellation)
    {
        private int _claimed;

        public async ValueTask<Turn> ClaimAsync()
        {
            if (Interlocked.Exchange(ref _claimed, 1) != 0)
                throw new InvalidOperationException(
                    "Actor barrier reservation was already consumed."
                );

            cancellation.Dispose();
            return await AwaitTurnAsync(waiter).ConfigureAwait(false);
        }

        public void Discard()
        {
            if (Interlocked.Exchange(ref _claimed, 1) != 0)
                return;
            cancellation.Cancel();
            cancellation.Dispose();
            _ = ReleaseIfAcquiredAsync(waiter);
        }

        private static async Task ReleaseIfAcquiredAsync(Waiter pending)
        {
            try
            {
                using var turn = await AwaitTurnAsync(pending).ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }
        }
    }

    public readonly struct Turn : IDisposable
    {
        private readonly Waiter? _waiter;

        internal Turn(Waiter waiter)
        {
            _waiter = waiter;
        }

        public void Dispose() => _waiter?.Release();
    }

    internal sealed class Waiter : IDisposable
    {
        private readonly ZLinkActorSerialExecutor _owner;
        private readonly CancellationToken _cancellationToken;
        private readonly TaskCompletionSource<Turn> _ready = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource _released = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly CancellationTokenRegistration _registration;
        private int _accepted;
        internal const int NotAccepted = 0;
        internal const int AcceptedWaiting = 1;
        private const int AcceptedStarted = 2;
        private ZLinkSerialWorkItem? _item;

        internal Waiter(
            ZLinkActorSerialExecutor owner,
            CancellationToken cancellationToken,
            bool countsAsPendingRequest
        )
        {
            _owner = owner;
            _cancellationToken = cancellationToken;
            CountsAsPendingRequest = countsAsPendingRequest;
            if (cancellationToken.CanBeCanceled)
                _registration = cancellationToken.Register(
                    static state => ((Waiter)state!).Cancel(),
                    this
                );
        }

        internal Task<Turn> Task => _ready.Task;
        internal bool CountsAsPendingRequest { get; }

        internal void MarkAccepted() => Volatile.Write(ref _accepted, AcceptedWaiting);

        internal void BindItem(ZLinkSerialWorkItem item)
        {
            _item = item;
            if (_cancellationToken.IsCancellationRequested)
                Cancel();
        }

        internal void FailPreparation(Exception error)
        {
            _ready.TrySetException(error);
        }

        internal bool TryStartAccepted() =>
            Interlocked.CompareExchange(ref _accepted, AcceptedStarted, AcceptedWaiting)
            == AcceptedWaiting;

        internal int FinishAccepted() => Interlocked.Exchange(ref _accepted, NotAccepted);

        internal async ValueTask RunAsync(CancellationToken _)
        {
            await _owner.OnWaiterStartedAsync(this).ConfigureAwait(false);
            if (_cancellationToken.IsCancellationRequested || !_ready.TrySetResult(new Turn(this)))
            {
                return;
            }
            if (_owner._queue.HasSharedConsumer)
            {
                var turn = ZLinkSerialTurn.Current!;
                turn.SignalSuspended();
            }
            await _released.Task.ConfigureAwait(false);
        }

        internal void Release()
        {
            if (!_owner._queue.HasSharedConsumer)
                _owner.OnWaiterFinished(this);
            _released.TrySetResult();
        }

        internal void DisposeCancellationRegistration() => _registration.Dispose();

        public void Dispose()
        {
            DisposeCancellationRegistration();
            Release();
        }

        private void Cancel()
        {
            _ready.TrySetException(new OperationCanceledException(_cancellationToken));
            if (Volatile.Read(ref _item) is { } item)
                _ = _owner._queue.WithdrawAsync(item);
        }
    }

    private sealed class MailboxFailureReporter : IZLinkRuntimeFailureReporter
    {
        internal static readonly MailboxFailureReporter Instance = new();

        public void ReportHandlerException(Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure("actor-dispatch-mailbox-handler", exception);
        }

        public void ReportUnhandledCallbackException(Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure("actor-dispatch-mailbox-callback", exception);
        }

        public void ReportRuntimeTaskException(string taskName, Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(taskName, exception);
        }
    }
}

internal sealed record ZLinkActorDurableRecord(
    Func<CancellationToken, ValueTask> Operation,
    Func<CancellationToken, ValueTask>? PrepareAsync = null,
    Action? CancelPreparation = null,
    Action? OnAdmitted = null,
    Action<Func<ValueTask<bool>>>? OnWithdrawable = null,
    Func<bool>? Ready = null,
    Action<Exception>? ReportFailure = null
);
