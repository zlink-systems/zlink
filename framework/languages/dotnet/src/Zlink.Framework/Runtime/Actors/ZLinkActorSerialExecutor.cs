using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Actors;

// Actor lifecycle policy over the runtime's common serial execution engine.
// Ordinary dispatch uses the application lane. A deferred Join barrier uses
// the lifecycle lane so it runs before already queued ordinary work, while a
// terminal barrier closes admission and joins the application lane after all
// work accepted before the close.
internal sealed class ZLinkActorSerialExecutor
{
    private readonly ZLinkStateLane _lane = new();
    private readonly ZLinkSerialExecutionQueue _queue;
    private bool _admissionClosed;
    private int _acceptedWaiters;
    private int _pendingRequests;

    public ZLinkActorSerialExecutor()
    {
        var reporter = MailboxFailureReporter.Instance;
        _queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(reporter, CancellationToken.None, this),
            reporter,
            CancellationToken.None);
    }

    public int PendingRequestCount
    {
        get => AwaitStateLane(_lane.RunAsync(() => _pendingRequests));
    }

    public ValueTask<Turn> EnterAsync(
        CancellationToken cancellationToken,
        bool countAsPendingRequest = false)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var waiter = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_admissionClosed)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    "Actor dispatch admission is closed for a terminal lifecycle transition.");
            return PostWaiterOnLane(
                ZLinkSerialWorkLane.Application,
                cancellationToken,
                countAsPendingRequest);
        }));
        return AwaitTurnAsync(waiter);
    }

    public BarrierReservation ReserveBarrier()
    {
        var cancellation = new CancellationTokenSource();
        var waiter = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_admissionClosed)
            {
                cancellation.Dispose();
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Actor lifecycle admission is closed for a terminal transition.");
            }
            return PostWaiterOnLane(
                ZLinkSerialWorkLane.Lifecycle,
                cancellation.Token,
                countAsPendingRequest: false);
        }));
        return new BarrierReservation(waiter, cancellation);
    }

    public BarrierReservation CloseAdmissionAndReserveLifecycleBarrier()
    {
        var cancellation = new CancellationTokenSource();
        var waiter = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_admissionClosed)
            {
                cancellation.Dispose();
                throw new InvalidOperationException(
                    "Actor terminal lifecycle barrier was already reserved.");
            }

            _admissionClosed = true;
            try
            {
                // Terminal cleanup must remain behind every ordinary turn that
                // was accepted before admission closed.
                return PostWaiterOnLane(
                    ZLinkSerialWorkLane.Application,
                    cancellation.Token,
                    countAsPendingRequest: false);
            }
            catch
            {
                _admissionClosed = false;
                cancellation.Dispose();
                throw;
            }
        }));
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
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_admissionClosed) return true;
            if (_acceptedWaiters != 0) return false;
            _admissionClosed = false;
            return true;
        }));
    }

    public void ReopenAdmission()
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_acceptedWaiters != 0)
                throw new InvalidOperationException(
                    "Actor dispatch admission cannot reopen while a previous lifecycle turn remains.");
            _admissionClosed = false;
        }));
    }

    private Waiter PostWaiterOnLane(
        ZLinkSerialWorkLane lane,
        CancellationToken cancellationToken,
        bool countAsPendingRequest)
    {
        var waiter = new Waiter(
            this,
            cancellationToken,
            countAsPendingRequest);
        if (countAsPendingRequest) _pendingRequests++;
        // Queue draining is a long-running task. It starts after this method returns, so
        // SuppressFlow is sufficient: TryPost only admits and schedules a cold task; it does
        // not synchronously invoke waiter.RunAsync.
        ZLinkSerialPostAdmission admission;
        using (ExecutionContext.SuppressFlow())
            admission = lane == ZLinkSerialWorkLane.Lifecycle
                ? _queue.TryPostNextWithAdmission(waiter.RunAsync, out _)
                : _queue.TryPostApplicationWithAdmission(waiter.RunAsync, out _);
        if (admission == ZLinkSerialPostAdmission.Accepted)
        {
            waiter.MarkAccepted();
            _acceptedWaiters++;
            return waiter;
        }

        if (countAsPendingRequest) _pendingRequests--;
        waiter.Dispose();
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ShuttingDown,
            "Actor dispatch queue is closed.");
    }

    private ValueTask OnWaiterStartedAsync(Waiter waiter) =>
        _lane.RunAsync(() =>
        {
            if (waiter.CountsAsPendingRequest) _pendingRequests--;
        });

    private void OnWaiterFinished(Waiter waiter)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!waiter.TryFinishAccepted()) return;
            _acceptedWaiters--;
        }));
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

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

    public sealed class BarrierReservation(
        Waiter waiter,
        CancellationTokenSource cancellation)
    {
        private int _claimed;

        public async ValueTask<Turn> ClaimAsync()
        {
            if (Interlocked.Exchange(ref _claimed, 1) != 0)
                throw new InvalidOperationException(
                    "Actor barrier reservation was already consumed.");

            cancellation.Dispose();
            return await AwaitTurnAsync(waiter).ConfigureAwait(false);
        }

        public void Discard()
        {
            if (Interlocked.Exchange(ref _claimed, 1) != 0) return;
            cancellation.Cancel();
            cancellation.Dispose();
            _ = ReleaseIfAcquiredAsync(waiter);
        }

        private static async Task ReleaseIfAcquiredAsync(Waiter pending)
        {
            try
            {
                using var turn = await AwaitTurnAsync(pending)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
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
        private readonly TaskCompletionSource<Turn> _ready =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource _released =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly CancellationTokenRegistration _registration;
        private int _accepted;
        private int _finished;
        private int _releasedOnce;

        internal Waiter(
            ZLinkActorSerialExecutor owner,
            CancellationToken cancellationToken,
            bool countsAsPendingRequest)
        {
            _owner = owner;
            _cancellationToken = cancellationToken;
            CountsAsPendingRequest = countsAsPendingRequest;
            if (cancellationToken.CanBeCanceled)
                _registration = cancellationToken.Register(
                    static state => ((Waiter)state!).Cancel(),
                    this);
        }

        internal Task<Turn> Task => _ready.Task;
        internal bool CountsAsPendingRequest { get; }

        internal void MarkAccepted() => Volatile.Write(ref _accepted, 1);

        internal bool TryFinishAccepted() =>
            Volatile.Read(ref _accepted) != 0
            && Interlocked.Exchange(ref _finished, 1) == 0;

        internal async ValueTask RunAsync(CancellationToken _)
        {
            await _owner.OnWaiterStartedAsync(this).ConfigureAwait(false);
            if (_cancellationToken.IsCancellationRequested
                || !_ready.TrySetResult(new Turn(this)))
            {
                _owner.OnWaiterFinished(this);
                return;
            }
            await _released.Task.ConfigureAwait(false);
        }

        internal void Release()
        {
            if (Interlocked.Exchange(ref _releasedOnce, 1) == 0)
            {
                _owner.OnWaiterFinished(this);
                _released.TrySetResult();
            }
        }

        internal void DisposeCancellationRegistration() =>
            _registration.Dispose();

        public void Dispose()
        {
            DisposeCancellationRegistration();
            Release();
        }

        private void Cancel() =>
            _ready.TrySetException(new OperationCanceledException());
    }

    private sealed class MailboxFailureReporter : IZLinkRuntimeFailureReporter
    {
        internal static readonly MailboxFailureReporter Instance = new();

        public void ReportHandlerException(Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "actor-dispatch-mailbox-handler",
                exception);
        }

        public void ReportUnhandledCallbackException(Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "actor-dispatch-mailbox-callback",
                exception);
        }

        public void ReportRuntimeTaskException(
            string taskName,
            Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(taskName, exception);
        }
    }
}
