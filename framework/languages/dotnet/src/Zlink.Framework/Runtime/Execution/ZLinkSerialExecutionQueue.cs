using System.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkSerialExecutionQueue : IAsyncDisposable
{
    private const string AdmissionOperationName = "execution";

    private const int RelocationJournalRecordHeaderBytes = sizeof(ulong) + sizeof(int);
    private const int SharedGateIdle = 0;
    private const int SharedGateOwned = 1;
    private const int SharedGateNotified = 2;
    private readonly object _admissionGate;
    private readonly object _disposeGate = new();

    private readonly TaskCompletionSource _drained = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );
    private TaskCompletionSource _applicationDrained = CompletedSignal();

    private readonly SemaphoreSlim? _drainGate;
    private ZLinkSerialExecutionQueue? _sharedOwner;
    private Func<ZLinkSerialExecutionQueue?>? _executionOwnerProvider;
    private ZLinkSerialWorkItem? _readyItem;
    private readonly Func<CancellationToken, ValueTask> _notifyShared;
    private readonly Action<ZLinkSerialWorkItem> _importSharedApplication;
    private readonly Action<ZLinkSerialWorkItem> _importSharedLifecycle;
    private readonly Func<Action, ValueTask> _dispatchTerminal;
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationToken _executionToken;
    private readonly ZLinkExecutionLanePolicy _policy;
    private readonly Func<ZLinkSerialWorkItem, bool>? _applicationStartAllowed;
    private readonly Func<ZLinkSerialWorkItem, bool> _applicationRunnable;

    // Instance method groups convert to a fresh delegate at every use site;
    // these run once per drained work item, so cache them.
    private readonly Func<
        ZLinkSerialTurn,
        Action<ZLinkSerialPostAdmission>,
        ZLinkSerialPostAdmission
    > _postResume;
    private readonly Func<Func<CancellationToken, ValueTask>, bool> _tryPostCallback;
    private readonly Action<Exception> _reportHandlerException;
    private readonly ZLinkSerialWorkQueue _applicationQueue = new();
    private readonly ZLinkSerialWorkQueue _lifecycleQueue = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private ZLinkSerialWorkItem? _active;
    private ZLinkSerialWorkItem? _activeLifecycle;
    private int _completed;
    private bool _applicationAdmissionClosed;
    private int _disposed;
    private Task? _disposeTask;
    private int _drainScheduled;
    private int _pendingCount;
    private int _applicationPendingCount;
    private int _lifecyclePendingCount;
    private int _consecutiveLifecycleTurns;
    private bool _lifecycleYieldDebt;
    private int _acceptedOperations;
    private ulong _nextClaimGeneration = 1;
    private ulong _activeClaimGeneration;
    private ulong _nextAcceptedSequence = 1;
    private ulong _nextRelocationSerial = 1;
    private ZLinkRelocationQueueState? _relocation;
    private TaskCompletionSource<ZLinkSerialRelocationSeal>? _sealRequest;
    private Func<int>? _sealRequestReservation;
    private bool _relocated;

    // Optional observation at the consumer boundary, before active-record claim.
    internal Action? ConsumerSelected { get; set; }
    internal Action<ZLinkSerialGateOperation>? GateOperation { get; set; }

    internal int ApplicationPendingCount
    {
        get
        {
            lock (_admissionGate)
                return _applicationPendingCount;
        }
    }

    internal int LifecyclePendingCount
    {
        get
        {
            lock (_admissionGate)
                return _lifecyclePendingCount;
        }
    }

    // A committed relocation moved this queue's work to the target.
    internal bool IsRelocated
    {
        get
        {
            lock (_admissionGate)
                return _relocated;
        }
    }

    internal Task ApplicationDrained
    {
        get
        {
            lock (_admissionGate)
                return _applicationDrained.Task;
        }
    }

    public ZLinkSerialExecutionQueue(
        ZLinkRuntimeTaskRunner taskRunner,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken executionToken
    )
        : this(taskRunner, errorSink, executionToken, ZLinkExecutionLanePolicy.Default) { }

    public ZLinkSerialExecutionQueue(
        ZLinkRuntimeTaskRunner taskRunner,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken executionToken,
        ZLinkExecutionLanePolicy policy,
        Func<ZLinkSerialWorkItem, bool>? applicationStartAllowed = null,
        bool sharedGate = false,
        ZLinkSerialExecutionQueue? sharedOwner = null
    )
    {
        ArgumentNullException.ThrowIfNull(taskRunner);
        ArgumentNullException.ThrowIfNull(errorSink);
        ArgumentNullException.ThrowIfNull(policy);
        _taskRunner = taskRunner;
        _errorSink = errorSink;
        _executionToken = executionToken;
        _policy = policy;
        _sharedOwner = sharedOwner ?? (sharedGate ? this : null);
        _admissionGate = sharedOwner?._admissionGate ?? new object();
        _drainGate = _sharedOwner is null ? new SemaphoreSlim(1, 1) : null;
        _readyItem = sharedOwner is null ? null : new ZLinkSerialWorkItem(this);
        _notifyShared = NotifySharedAsync;
        _importSharedApplication = ImportSharedApplication;
        _importSharedLifecycle = ImportSharedLifecycle;
        _dispatchTerminal = DispatchTerminalAsync;
        _applicationStartAllowed = applicationStartAllowed;
        _applicationRunnable = IsApplicationRunnable;
        _postResume = PostResume;
        _tryPostCallback = TryPostCallback;
        _reportHandlerException = ReportHandlerException;
    }

    internal bool HasSharedConsumer => _sharedOwner is not null;

    internal void BindExecutionOwner(Func<ZLinkSerialExecutionQueue?> provider)
    {
        ArgumentNullException.ThrowIfNull(provider);
        lock (_admissionGate)
            _executionOwnerProvider = provider;
    }

    // The current consumer transfers an idle mailbox before the next claim.
    // Accepted records retain FIFO and their terminal owner during a turn.
    private bool RedirectIdleMailbox()
    {
        if (_executionOwnerProvider is null || _active is not null || _activeLifecycle is not null)
            return false;
        var owner = _executionOwnerProvider();
        if (ReferenceEquals(owner, _sharedOwner))
            return false;
        lock (_admissionGate)
        {
            if (_sharedOwner is not null)
                ImportSharedPublished();
            _readyItem ??= new ZLinkSerialWorkItem(this);
            Volatile.Write(ref _sharedOwner, owner);
        }
        return true;
    }

    public ValueTask DisposeAsync()
    {
        Task task;
        TaskCompletionSource? start = null;
        lock (_disposeGate)
        {
            if (_disposeTask is null)
            {
                Volatile.Write(ref _disposed, 1);
                start = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                _disposeTask = DisposeCoreAsync(start.Task);
            }
            task = _disposeTask;
        }
        start?.TrySetResult();
        return new ValueTask(task);
    }

    private async Task DisposeCoreAsync(Task started)
    {
        await started.ConfigureAwait(false);
        Complete();
        try
        {
            await _drained.Task.ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        catch (ObjectDisposedException) { }

        _drainGate?.Dispose();
    }

    public void Complete()
    {
        if (_sharedOwner is not null)
        {
            lock (_admissionGate)
            {
                if (Interlocked.Exchange(ref _completed, 1) != 0)
                    return;
            }
            _ = PostSharedControl(() =>
            {
                TaskCompletionSource<ZLinkSerialRelocationSeal>? pending;
                lock (_admissionGate)
                {
                    pending = _sealRequest;
                    _sealRequest = null;
                    _sealRequestReservation = null;
                    AbortRelocationUnderLock();
                }
                pending?.TrySetException(
                    new InvalidOperationException(
                        "ZLink serial execution queue closed before relocation seal completed."
                    )
                );
                ImportSharedPublished();
                RegisterSharedHead();
                TrySignalDrained();
            });
            return;
        }
        TaskCompletionSource<ZLinkSerialRelocationSeal>? pendingSeal;
        Func<CancellationToken, ValueTask>? drain = null;
        lock (_admissionGate)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0)
                return;
            pendingSeal = _sealRequest;
            _sealRequest = null;
            _sealRequestReservation = null;
            AbortRelocationUnderLock();
            if (_applicationQueue.Count > 0 || _lifecycleQueue.Count > 0)
                drain = ReserveDrainUnderLock();
        }
        PublishDrain(drain);
        pendingSeal?.TrySetException(
            new InvalidOperationException(
                "ZLink serial execution queue closed before relocation seal completed."
            )
        );
        TrySignalDrained();
    }

    internal void CloseApplicationAdmission()
    {
        lock (_admissionGate)
            _applicationAdmissionClosed = true;
    }

    public ValueTask<ZLinkSerialWorkItem> PostAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken
    ) =>
        PostAsync(
            callback,
            payloadBytes: 0,
            metadataBytes: 0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            cancellationToken
        );

    internal ValueTask<ZLinkSerialWorkItem> PostAsync(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        CancellationToken cancellationToken,
        Func<bool>? ready = null
    )
    {
        cancellationToken.ThrowIfCancellationRequested();

        var admission = TryPostApplicationWithAdmission(
            callback,
            payloadBytes,
            metadataBytes,
            transferred,
            out var item,
            ready
        );
        if (admission != ZLinkSerialPostAdmission.Accepted)
            throw CreateAdmissionException(AdmissionOperationName, admission);
        return ValueTask.FromResult(item);
    }

    public bool TryPost(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    ) => TryPostApplicationWithAdmission(callback, out item) == ZLinkSerialPostAdmission.Accepted;

    public bool TryPostApplication(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    ) => TryPostApplicationWithAdmission(callback, out item) == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission TryPostApplicationWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    ) =>
        TryPostApplicationWithAdmission(
            callback,
            payloadBytes: 0,
            metadataBytes: 0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            out item
        );

    internal ZLinkSerialPostAdmission TryPostApplicationWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        out ZLinkSerialWorkItem item,
        Func<bool>? ready = null
    )
    {
        var admission = TryPostApplicationWithAdmission(
            callback,
            payloadBytes,
            metadataBytes,
            transferred,
            out item,
            out var drain,
            ready
        );
        PublishDrain(drain);
        return admission;
    }

    private ZLinkSerialPostAdmission TryPostApplicationWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        out ZLinkSerialWorkItem item,
        out Func<CancellationToken, ValueTask>? drain,
        Func<bool>? ready = null
    )
    {
        drain = null;
        ArgumentNullException.ThrowIfNull(callback);
        if (payloadBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(payloadBytes));
        if (metadataBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(metadataBytes));
        var candidate = new ZLinkSerialWorkItem(
            callback,
            lane: ZLinkSerialWorkLane.Application,
            ready: ready
        );
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0 || _applicationAdmissionClosed)
            {
                item = null!;
                return ZLinkSerialPostAdmission.Closed;
            }
            CommitWorkItemUnderLock(_applicationQueue, candidate, ZLinkSerialWorkLane.Application);
            item = candidate;
            drain = ReserveDrainUnderLock();
        }

        if (transferred)
            _ = ZLinkApplicationJobQueueInvocation.TryTransferOwnerReservation();
        return ZLinkSerialPostAdmission.Accepted;
    }

    public bool TryPostNext(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    ) => TryPostNextWithAdmission(callback, out item) == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission TryPostNextWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    ) =>
        TryPostNextWithAdmission(
            callback,
            payloadBytes: 0,
            metadataBytes: 0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            out item
        );

    internal ZLinkSerialPostAdmission TryPostNextWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        out ZLinkSerialWorkItem item
    )
    {
        var admission = TryPostNextWithAdmission(
            callback,
            payloadBytes,
            metadataBytes,
            transferred,
            out item,
            out var drain
        );
        PublishDrain(drain);
        return admission;
    }

    private ZLinkSerialPostAdmission TryPostNextWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        out ZLinkSerialWorkItem item,
        out Func<CancellationToken, ValueTask>? drain
    )
    {
        drain = null;
        ArgumentNullException.ThrowIfNull(callback);
        if (payloadBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(payloadBytes));
        if (metadataBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(metadataBytes));
        var candidate = new ZLinkSerialWorkItem(callback, lane: ZLinkSerialWorkLane.Lifecycle);
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return ZLinkSerialPostAdmission.Closed;
            }
            CommitWorkItemUnderLock(_lifecycleQueue, candidate, ZLinkSerialWorkLane.Lifecycle);
            item = candidate;
            drain = ReserveDrainUnderLock();
        }

        if (transferred)
            _ = ZLinkApplicationJobQueueInvocation.TryTransferOwnerReservation();
        return ZLinkSerialPostAdmission.Accepted;
    }

    public ZLinkAcceptedWorkAdmission TryPostAccepted(
        ReadOnlyMemory<byte> payload,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        out ZLinkSerialWorkItem item
    ) =>
        TryPostAccepted(
            payload,
            callback,
            relocationRelease,
            previousOwnerMessageFollow: false,
            out item
        );

    public ZLinkAcceptedWorkAdmission TryPostAccepted(
        ReadOnlyMemory<byte> payload,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out ZLinkSerialWorkItem item
    ) =>
        TryPostAcceptedCore(
            payload.Length,
            null,
            payload,
            callback,
            relocationRelease,
            previousOwnerMessageFollow,
            out item
        );

    internal ZLinkAcceptedWorkAdmission TryPostAccepted(
        int payloadLength,
        Func<ReadOnlyMemory<byte>> payloadFactory,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out ZLinkSerialWorkItem item,
        object? acceptedState = null
    )
    {
        ArgumentNullException.ThrowIfNull(payloadFactory);
        return TryPostAcceptedCore(
            payloadLength,
            payloadFactory,
            default,
            callback,
            relocationRelease,
            previousOwnerMessageFollow,
            out item,
            acceptedState
        );
    }

    private ZLinkAcceptedWorkAdmission TryPostAcceptedCore(
        int payloadLength,
        Func<ReadOnlyMemory<byte>>? payloadFactory,
        ReadOnlyMemory<byte> payload,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out ZLinkSerialWorkItem item,
        object? acceptedState = null
    )
    {
        ArgumentNullException.ThrowIfNull(callback);
        ArgumentNullException.ThrowIfNull(relocationRelease);
        if (payloadLength < 0)
            throw new ArgumentOutOfRangeException(nameof(payloadLength));
        EnsureRelocationRecordLength(payloadLength);
        Func<CancellationToken, ValueTask>? drain;

        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0 || _applicationAdmissionClosed)
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.Closed;
            }
            if (_relocated || _relocation?.IngressFrozen == true)
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.RelocationMoving;
            }
            TryCommitAcceptedWorkUnderLock(
                callback,
                relocationRelease,
                previousOwnerMessageFollow,
                payloadLength,
                payload,
                payloadFactory,
                out item,
                out var scheduleDrain,
                acceptedState
            );
            drain = scheduleDrain ? ReserveDrainUnderLock() : null;
        }

        _ = ZLinkApplicationJobQueueInvocation.TryTransferOwnerReservation();
        PublishDrain(drain);
        return ZLinkAcceptedWorkAdmission.Accepted;
    }

    private static void EnsureRelocationRecordLength(int payloadLength) =>
        _ = checked(RelocationJournalRecordHeaderBytes + (long)payloadLength);

    public bool TryPostFinal(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item
    )
    {
        ArgumentNullException.ThrowIfNull(callback);
        // The terminal closes admission, but it must remain behind every
        // application turn that was already accepted. Placing it in the
        // lifecycle lane would let lifecycle priority overtake and dispose
        // those accepted turns.
        var candidate = new ZLinkSerialWorkItem(callback, lane: ZLinkSerialWorkLane.Application);
        Func<CancellationToken, ValueTask>? drain;
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return false;
            }

            if (_sharedOwner is null)
                AbortRelocationUnderLock();
            else
                candidate.PreparePublication = () =>
                {
                    lock (_admissionGate)
                        AbortRelocationUnderLock();
                };
            CommitWorkItemUnderLock(_applicationQueue, candidate, ZLinkSerialWorkLane.Application);
            Volatile.Write(ref _completed, 1);
            item = candidate;
            drain = ReserveDrainUnderLock();
        }
        PublishDrain(drain);
        return true;
    }

    internal ValueTask<ZLinkSerialRelocationSeal?> TrySealRelocationAsync() =>
        RunOnSharedGateAsync(() => TrySealRelocationOnGate(out var seal) ? seal : null);

    internal ValueTask<(
        bool Succeeded,
        ZLinkSerialRelocationSeal Seal,
        ulong FirstReservedSequence
    )> TrySealRelocationAsync(
        int reservedAcceptedSequences,
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit
    ) =>
        RunOnSharedGateAsync(() =>
        {
            var succeeded = TrySealRelocationOnGate(
                reservedAcceptedSequences,
                admit,
                out var seal,
                out var first
            );
            return (succeeded, seal, first);
        });

    internal ValueTask<bool> TryAbortRelocationAsync(ZLinkSerialRelocationSeal seal) =>
        RunOnSharedGateAsync(() => TryAbortRelocationOnGate(seal));

    internal ValueTask<bool> TryOpenRelocationAfterMessageFollowAsync(
        ZLinkSerialRelocationSeal seal
    ) => RunOnSharedGateAsync(() => TryOpenRelocationAfterMessageFollowOnGate(seal));

    internal ValueTask<(
        bool Succeeded,
        IReadOnlyList<ZLinkAcceptedWorkRecord> Held
    )> TryCommitRelocationAsync(ZLinkSerialRelocationSeal seal) =>
        RunOnSharedGateAsync(() =>
        {
            var succeeded = TryCommitRelocationOnGate(seal, out var held);
            return (succeeded, held);
        });

    internal ValueTask<(
        bool Succeeded,
        IReadOnlyList<ZLinkAcceptedWorkRecord> Held
    )> TryFreezeRelocationIngressAsync(ZLinkSerialRelocationSeal seal) =>
        RunOnSharedGateAsync(() =>
        {
            var succeeded = TryFreezeRelocationIngressOnGate(seal, out var held);
            return (succeeded, held);
        });

    internal ValueTask<bool> HasPendingAcceptedStateOrCloseApplicationAdmissionAsync(
        Func<object, bool> predicate
    ) =>
        RunOnSharedGateAsync(() =>
            HasPendingAcceptedStateOrCloseApplicationAdmissionOnGate(predicate)
        );

    private bool TrySealRelocationOnGate(out ZLinkSerialRelocationSeal seal)
    {
        RequireSharedGate();
        lock (_admissionGate)
        {
            if (_sharedOwner is not null)
                ImportSharedPublished();
            if (
                _relocated
                || _relocation is not null
                || _active is not null
                || _activeLifecycle is not null
                || _acceptedOperations != 0
                || _sealRequest is not null
                || Volatile.Read(ref _completed) != 0
            )
            {
                seal = null!;
                return false;
            }
            seal = SealUnderLock();
            return true;
        }
    }

    private bool TrySealRelocationOnGate(
        int reservedAcceptedSequences,
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        out ZLinkSerialRelocationSeal seal,
        out ulong firstReservedSequence
    )
    {
        RequireSharedGate();
        ArgumentNullException.ThrowIfNull(admit);
        if (reservedAcceptedSequences < 0)
            throw new ArgumentOutOfRangeException(nameof(reservedAcceptedSequences));
        lock (_admissionGate)
        {
            if (_sharedOwner is not null)
                ImportSharedPublished();
            if (
                _relocated
                || _relocation is not null
                || _active is not null
                || _activeLifecycle is not null
                || _acceptedOperations != 0
                || _sealRequest is not null
                || Volatile.Read(ref _completed) != 0
            )
            {
                seal = null!;
                firstReservedSequence = 0;
                return false;
            }
            var captured = _applicationQueue
                .Where(static item => item.IsAccepted)
                .Select(static item => item.CreateAcceptedRecord())
                .ToArray();
            if (!admit(captured))
            {
                seal = null!;
                firstReservedSequence = 0;
                return false;
            }
            seal = SealUnderLock(reservedAcceptedSequences, captured);
            firstReservedSequence = seal.FirstReservedSequence;
            return true;
        }
    }

    public ValueTask<ZLinkSerialRelocationSeal> SealRelocationAsync(
        CancellationToken cancellationToken
    ) => SealRelocationAsync(static () => 0, cancellationToken);

    internal ValueTask<ZLinkSerialRelocationSeal> SealRelocationAsync(
        Func<int> reserveAcceptedSequencesAtBoundary,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        ArgumentNullException.ThrowIfNull(reserveAcceptedSequencesAtBoundary);
        TaskCompletionSource<ZLinkSerialRelocationSeal> request;
        Func<CancellationToken, ValueTask>? drain = null;
        lock (_admissionGate)
        {
            if (_relocated)
                throw new InvalidOperationException(
                    "ZLink serial queue owner has already relocated."
                );
            if (_relocation is not null)
                throw new InvalidOperationException(
                    "ZLink serial queue owner is already sealed for relocation."
                );
            if (Volatile.Read(ref _completed) != 0)
                throw new InvalidOperationException("ZLink serial execution queue is closed.");
            if (_sealRequest is not null)
                throw new InvalidOperationException(
                    "ZLink serial execution queue already has a pending relocation seal."
                );

            request = new TaskCompletionSource<ZLinkSerialRelocationSeal>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            _sealRequest = request;
            _sealRequestReservation = reserveAcceptedSequencesAtBoundary;
            if (
                _sharedOwner is null
                && _active is null
                && _activeLifecycle is null
                && _acceptedOperations == 0
            )
                CompleteSealRequestUnderLock();
            else
                drain = ReserveDrainUnderLock();
        }
        PublishDrain(drain);
        return AwaitSealRequestAsync(request, cancellationToken);
    }

    private async ValueTask<ZLinkSerialRelocationSeal> AwaitSealRequestAsync(
        TaskCompletionSource<ZLinkSerialRelocationSeal> request,
        CancellationToken cancellationToken
    )
    {
        try
        {
            return await request.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            if (!IsInsideSharedGate)
            {
                await DispatchTerminalAsync(() => CancelSealRequest(request)).ConfigureAwait(false);
                throw;
            }
            CancelSealRequest(request);
            throw;
        }
    }

    private void CancelSealRequest(TaskCompletionSource<ZLinkSerialRelocationSeal> request)
    {
        Func<CancellationToken, ValueTask>? drain = null;
        lock (_admissionGate)
        {
            if (ReferenceEquals(_sealRequest, request))
            {
                _sealRequest = null;
                _sealRequestReservation = null;
                drain = ReserveDrainUnderLock();
            }
            else if (request.Task.IsCompletedSuccessfully && Matches(request.Task.Result))
            {
                AbortRelocationUnderLock();
                drain = ReserveDrainUnderLock();
            }
        }
        PublishDrain(drain);
    }

    internal bool TryAbortRelocationOnGate(ZLinkSerialRelocationSeal seal)
    {
        RequireSharedGate();
        ArgumentNullException.ThrowIfNull(seal);
        Func<CancellationToken, ValueTask>? drain;
        lock (_admissionGate)
        {
            if (!Matches(seal))
                return false;
            AbortRelocationUnderLock();
            drain = ReserveDrainUnderLock();
        }
        PublishDrain(drain);
        return true;
    }

    internal bool TryOpenRelocationAfterMessageFollowOnGate(ZLinkSerialRelocationSeal seal)
    {
        RequireSharedGate();
        ArgumentNullException.ThrowIfNull(seal);
        Func<CancellationToken, ValueTask>? drain;
        lock (_admissionGate)
        {
            if (!Matches(seal))
                return false;
            var relocation = _relocation!;
            if (_sharedOwner is not null)
                relocation.Held.ImportPublished();
            var direct = new ZLinkSerialWorkQueue();
            while (relocation.Captured.TryDequeue(out var item))
                _applicationQueue.Enqueue(item);
            while (relocation.Held.TryDequeue(out var item))
            {
                if (item.PreviousOwnerMessageFollow)
                    RestoreHeldItemUnderGate(item);
                else
                    direct.Enqueue(item);
            }
            while (direct.TryDequeue(out var item))
                RestoreHeldItemUnderGate(item);
            _relocation = null;
            drain = ReserveDrainUnderLock();
        }
        PublishDrain(drain);
        return true;
    }

    private void AbortRelocationUnderLock()
    {
        if (_relocation is null)
            return;
        if (_sharedOwner is not null)
            _relocation.Held.ImportPublished();
        while (_relocation.Captured.TryDequeue(out var item))
            _applicationQueue.Enqueue(item);
        while (_relocation.Held.TryDequeue(out var item))
            RestoreHeldItemUnderGate(item);
        _relocation = null;
    }

    private void RestoreHeldItemUnderGate(ZLinkSerialWorkItem item)
    {
        if (_sharedOwner is null)
            _applicationQueue.Enqueue(item);
        else
            _applicationQueue.Adopt(item);
    }

    internal bool TryCommitRelocationOnGate(
        ZLinkSerialRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held
    )
    {
        RequireSharedGate();
        ArgumentNullException.ThrowIfNull(seal);
        ZLinkSerialWorkItem[] released;
        lock (_admissionGate)
        {
            if (!Matches(seal))
            {
                held = [];
                return false;
            }

            if (_sharedOwner is not null)
                _relocation!.Held.ImportPublished();
            held = _relocation!.Held.Select(static item => item.CreateAcceptedRecord()).ToArray();
            released = _relocation.Captured.Concat(_relocation.Held).ToArray();
            _relocation.Captured.Clear();
            _relocation.Held.Clear();
            _relocation = null;
            _relocated = true;
        }

        foreach (var item in released)
            item.ReleaseForRelocation(ReportHandlerException);
        return true;
    }

    internal bool TryFreezeRelocationIngressOnGate(
        ZLinkSerialRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held
    )
    {
        RequireSharedGate();
        ArgumentNullException.ThrowIfNull(seal);
        lock (_admissionGate)
        {
            if (!Matches(seal))
            {
                held = [];
                return false;
            }
            _relocation!.IngressFrozen = true;
            if (_sharedOwner is not null)
                _relocation.Held.ImportPublished();
            held = _relocation.Held.Select(static item => item.CreateAcceptedRecord()).ToArray();
            return true;
        }
    }

    private bool Matches(ZLinkSerialRelocationSeal seal)
    {
        return _relocation is not null && _relocation.Serial == seal.Serial;
    }

    private void CommitWorkItemUnderLock(
        ZLinkSerialWorkQueue destination,
        ZLinkSerialWorkItem item,
        ZLinkSerialWorkLane lane
    )
    {
        if (!item.ReservationHeld)
        {
            item.BindTerminalRelease(() => CompletePendingItem(item));
            PublishWorkUnderLock(destination, item);
            return;
        }
        // This queue owns ordering only. The queued callback retains the
        // actual Core receive owner until its terminal path disposes it.
        var applicationDrained = NewApplicationDrainedSignalUnderLock(lane);
        item.BindTerminalRelease(() => CompletePendingItem(item));
        PublishWorkUnderLock(destination, item);
        CommitReservationUnderLock(lane, applicationDrained);
    }

    private void PublishWorkUnderLock(ZLinkSerialWorkQueue destination, ZLinkSerialWorkItem item)
    {
        if (_sharedOwner is null)
        {
            destination.Enqueue(item);
            return;
        }
        destination.Publish(item);
        if (_readyItem is not null)
            PublishSharedNotificationUnderLock();
    }

    private void PublishSharedNotificationUnderLock()
    {
        if (_executionOwnerProvider is null)
            _sharedOwner!._applicationQueue.Publish(new ZLinkSerialWorkItem(this));
    }

    private void TryCommitAcceptedWorkUnderLock(
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        int payloadLength,
        ReadOnlyMemory<byte> payload,
        Func<ReadOnlyMemory<byte>>? payloadFactory,
        out ZLinkSerialWorkItem item,
        out bool scheduleDrain,
        object? acceptedState = null
    )
    {
        if (_nextAcceptedSequence == ulong.MaxValue)
            throw new InvalidOperationException("ZLink accepted-work sequence is exhausted.");

        var acceptedSequence = _nextAcceptedSequence;
        var destination = _relocation?.Held ?? _applicationQueue;
        var candidate = new ZLinkSerialWorkItem(
            callback,
            relocationRelease,
            previousOwnerMessageFollow,
            ZLinkSerialWorkLane.Application,
            acceptedSequence,
            payload,
            payloadFactory
        );
        candidate.AcceptedState = acceptedState;

        CommitWorkItemUnderLock(destination, candidate, ZLinkSerialWorkLane.Application);
        _nextAcceptedSequence = acceptedSequence + 1;

        item = candidate;
        scheduleDrain = _relocation is null;
    }

    private void CommitReservationUnderLock(
        ZLinkSerialWorkLane lane,
        TaskCompletionSource? applicationDrained = null
    )
    {
        ref var pendingCount = ref PendingCount(lane);
        if (applicationDrained is not null)
            _applicationDrained = applicationDrained;
        pendingCount++;
        _pendingCount++;
    }

    private TaskCompletionSource? NewApplicationDrainedSignalUnderLock(ZLinkSerialWorkLane lane) =>
        lane == ZLinkSerialWorkLane.Application && _applicationPendingCount == 0
            ? new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
            : null;

    private void ReleaseReservedSlotUnderLock(ZLinkSerialWorkLane lane)
    {
        ref var pendingCount = ref PendingCount(lane);
        pendingCount--;
        _pendingCount--;
        if (lane == ZLinkSerialWorkLane.Application && pendingCount == 0)
            _applicationDrained.TrySetResult();
    }

    private ref int PendingCount(ZLinkSerialWorkLane lane)
    {
        if (lane == ZLinkSerialWorkLane.Application)
            return ref _applicationPendingCount;
        return ref _lifecyclePendingCount;
    }

    private static TaskCompletionSource CompletedSignal()
    {
        var signal = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        signal.TrySetResult();
        return signal;
    }

    public async ValueTask RunAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken
    )
    {
        var completion = RunAsync(callback, cancellationToken, out var drain);
        PublishDrain(drain);
        await completion.ConfigureAwait(false);
    }

    internal ValueTask RunAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        out Func<CancellationToken, ValueTask>? drain
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var admission = TryPostApplicationWithAdmission(
            callback,
            0,
            0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            out var item,
            out drain
        );
        if (admission != ZLinkSerialPostAdmission.Accepted)
            throw CreateAdmissionException(AdmissionOperationName, admission);
        return new ValueTask(item.Completion.WaitAsync(cancellationToken));
    }

    internal async ValueTask RunLifecycleAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken
    )
    {
        var completion = RunLifecycleAsync(callback, cancellationToken, out var drain);
        PublishDrain(drain);
        await completion.ConfigureAwait(false);
    }

    internal ValueTask RunLifecycleAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        out Func<CancellationToken, ValueTask>? drain
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var admission = TryPostNextWithAdmission(
            callback,
            0,
            0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            out var item,
            out drain
        );
        if (admission != ZLinkSerialPostAdmission.Accepted)
            throw CreateAdmissionException("lifecycle", admission);
        return new ValueTask(item.Completion.WaitAsync(cancellationToken));
    }

    private static ZLinkFrameworkException CreateAdmissionException(
        string lane,
        ZLinkSerialPostAdmission admission
    )
    {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ShuttingDown,
            $"The serial {lane} queue is closed."
        );
    }

    private Func<CancellationToken, ValueTask>? ReserveDrainUnderLock()
    {
        if (_sharedOwner is not null)
            return _notifyShared;
        if (
            _drainScheduled != 0
            || (
                !HasRunnableApplicationUnderLock()
                && _activeLifecycle?.ReadyContinuation is null
                && (_activeLifecycle is not null || _lifecycleQueue.Count == 0)
            )
        )
            return null;
        Volatile.Write(ref _drainScheduled, 1);
        return DrainAsync;
    }

    internal void PublishDrain(Func<CancellationToken, ValueTask>? drain)
    {
        if (drain is null)
            return;
        if (_sharedOwner is not null)
        {
            _ = drain(CancellationToken.None);
            return;
        }
        if (!_taskRunner.TryRunDetached("serial-queue-drain", drain))
            _ = Task.Run(
                async () => await drain(CancellationToken.None).ConfigureAwait(false),
                CancellationToken.None
            );
    }

    internal void SchedulePendingWork()
    {
        Func<CancellationToken, ValueTask>? drain;
        lock (_admissionGate)
        {
            if (_readyItem is not null)
                PublishSharedNotificationUnderLock();
            drain = ReserveDrainUnderLock();
        }
        PublishDrain(drain);
    }

    internal void NotifyReadiness()
    {
        if (_sharedOwner is null)
            return;
        lock (_admissionGate)
            if (_readyItem is not null)
                PublishSharedNotificationUnderLock();
        _ = NotifySharedAsync(CancellationToken.None);
    }

    private ValueTask NotifySharedAsync(CancellationToken cancellationToken)
    {
        var owner = Volatile.Read(ref _sharedOwner);
        if (owner is null)
        {
            Func<CancellationToken, ValueTask>? drain;
            lock (_admissionGate)
                drain = ReserveDrainUnderLock();
            PublishDrain(drain);
            return ValueTask.CompletedTask;
        }
        if (_executionOwnerProvider is not null)
            lock (owner._admissionGate)
                owner._applicationQueue.Publish(new ZLinkSerialWorkItem(this));
        owner.GateOperation?.Invoke(ZLinkSerialGateOperation.Acquire);
        if (
            Interlocked.CompareExchange(ref owner._drainScheduled, SharedGateOwned, SharedGateIdle)
            == SharedGateIdle
        )
            owner.ScheduleSharedDrain();
        else
        {
            // 2 records a publication made while the one gate owner is busy.
            // Its failed idle transition forces another acquire scan, so a
            // publisher cannot lose a wakeup at the end of a drain.
            owner.GateOperation?.Invoke(ZLinkSerialGateOperation.Notify);
            if (
                Interlocked.CompareExchange(
                    ref owner._drainScheduled,
                    SharedGateNotified,
                    SharedGateOwned
                ) == SharedGateIdle
            )
                _ = owner.NotifySharedAsync(cancellationToken);
        }
        return ValueTask.CompletedTask;
    }

    private void ScheduleSharedDrain()
    {
        if (!_taskRunner.TryRunDetached("serial-queue-drain", DrainSharedAsync))
            _ = Task.Run(async () =>
                await DrainSharedAsync(CancellationToken.None).ConfigureAwait(false)
            );
    }

    private void ImportSharedApplication(ZLinkSerialWorkItem item)
    {
        item.PreparePublication?.Invoke();
        if (item.Mailbox is { } mailbox)
        {
            if (!ReferenceEquals(mailbox._sharedOwner, this))
                return;
            mailbox.ImportSharedPublished();
            mailbox.RegisterSharedHead();
        }
        else
            _applicationQueue.Enqueue(item);
    }

    private void ImportSharedLifecycle(ZLinkSerialWorkItem item)
    {
        if (item.LifecycleOwner is { } owner)
        {
            if (ReferenceEquals(_activeLifecycle, owner) && owner.ReadyContinuation is null)
                owner.ReadyContinuation = item;
            else
            {
                item.RejectPublication?.Invoke();
                item.ReleaseForRelocation(_reportHandlerException);
            }
        }
        else
            _lifecycleQueue.Enqueue(item);
    }

    private void ImportSharedPublished()
    {
        _applicationQueue.ImportPublished(_importSharedApplication);
        _lifecycleQueue.ImportPublished(_importSharedLifecycle);
    }

    private void RegisterSharedHead()
    {
        if (_readyItem is not { Next: null } ready)
            return;
        if (
            HasRunnableApplicationUnderLock()
            || _activeLifecycle?.ReadyContinuation is not null
            || _activeLifecycle is null && _lifecycleQueue.Count != 0
        )
            _sharedOwner!._applicationQueue.Enqueue(ready);
    }

    private async ValueTask DrainSharedAsync(CancellationToken cancellationToken)
    {
        var sliceStartedAt = Stopwatch.GetTimestamp();
        while (true)
        {
            ImportSharedPublished();
            if (TryTakeNextCore(out var selected, out _))
            {
                var mailbox = selected.Mailbox ?? this;
                var item = selected;
                if (selected.Mailbox is not null)
                {
                    if (!ReferenceEquals(mailbox._sharedOwner, this))
                        continue;
                    if (mailbox.RedirectIdleMailbox())
                    {
                        Func<CancellationToken, ValueTask>? redirected;
                        lock (mailbox._admissionGate)
                            redirected = mailbox.ReserveDrainUnderLock();
                        mailbox.PublishDrain(redirected);
                        continue;
                    }
                    if (!mailbox.TryTakeNextCore(out item, out _))
                        continue;
                }
                var turn = new ZLinkSerialTurn(
                    mailbox._postResume,
                    mailbox._tryPostCallback,
                    mailbox._reportHandlerException,
                    mailbox._executionToken,
                    item,
                    this
                );
                await item.InvokeAsync(
                        mailbox._reportHandlerException,
                        mailbox._executionToken,
                        turn
                    )
                    .ConfigureAwait(false);
                if (mailbox._readyItem is null)
                    mailbox._active = null;
                mailbox.CompleteSharedSealRequest();
                mailbox.RegisterSharedHead();
                if (Stopwatch.GetElapsedTime(sliceStartedAt) >= _policy.OwnerTimeBudget)
                {
                    ScheduleSharedDrain();
                    return;
                }
                continue;
            }
            // A producer either marks this owner notified or wins the idle
            // CAS itself. There is no independent Actor drain reservation.
            GateOperation?.Invoke(ZLinkSerialGateOperation.Release);
            if (
                Interlocked.CompareExchange(ref _drainScheduled, SharedGateIdle, SharedGateOwned)
                == SharedGateOwned
            )
            {
                TrySignalDrained();
                return;
            }
            GateOperation?.Invoke(ZLinkSerialGateOperation.Scan);
            Interlocked.CompareExchange(ref _drainScheduled, SharedGateOwned, SharedGateNotified);
        }
    }

    internal bool IsInsideSharedGate =>
        _sharedOwner is null
        || ZLinkSerialTurn.Current is { } turn
            && ReferenceEquals(turn.ExecutionGate, _sharedOwner)
            && !turn.Suspended.IsCompleted;

    private void RequireSharedGate()
    {
        if (!IsInsideSharedGate)
            throw new InvalidOperationException("Shared queue state requires its execution gate.");
    }

    internal async ValueTask<T> RunOnSharedGateAsync<T>(Func<T> operation)
    {
        if (IsInsideSharedGate)
            return operation();
        T result = default!;
        var item = PostSharedControl(() => result = operation());
        await item.Completion.ConfigureAwait(false);
        return result;
    }

    private ZLinkSerialWorkItem PostSharedControl(Action operation)
    {
        var owner = _sharedOwner!;
        var item = new ZLinkSerialWorkItem(
            _ =>
            {
                operation();
                return ValueTask.CompletedTask;
            },
            reservationHeld: false
        );
        lock (owner._admissionGate)
            owner.CommitWorkItemUnderLock(
                owner._applicationQueue,
                item,
                ZLinkSerialWorkLane.Application
            );
        _ = owner.NotifySharedAsync(CancellationToken.None);
        return item;
    }

    private ValueTask DispatchTerminalAsync(Action operation) =>
        new(PostSharedControl(operation).Completion);

    private void CompleteSharedSealRequest()
    {
        if (_sealRequest is not null)
            lock (_admissionGate)
                CompleteSealRequestUnderLock();
    }

    private void ReleaseDrain()
    {
        Func<CancellationToken, ValueTask>? drain;
        lock (_admissionGate)
        {
            Volatile.Write(ref _drainScheduled, 0);
            drain =
                _applicationQueue.Count > 0
                || _lifecycleQueue.Count > 0
                || _activeLifecycle?.ReadyContinuation is not null
                    ? ReserveDrainUnderLock()
                    : null;
        }
        PublishDrain(drain);
        TrySignalDrained();
    }

    private async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        if (!await _drainGate!.WaitAsync(0, CancellationToken.None).ConfigureAwait(false))
            return;

        try
        {
            var sliceStartedAt = Stopwatch.GetTimestamp();
            while (true)
            {
                if (RedirectIdleMailbox())
                {
                    GateOperation?.Invoke(ZLinkSerialGateOperation.ConsumerTransfer);
                    break;
                }
                if (!TryTakeNext(out var item, out var claimGeneration))
                    break;
                var turn = new ZLinkSerialTurn(
                    _postResume,
                    _tryPostCallback,
                    _reportHandlerException,
                    _executionToken,
                    item
                );
                await item.InvokeAsync(_reportHandlerException, _executionToken, turn)
                    .ConfigureAwait(false);
                lock (_admissionGate)
                {
                    if (ReferenceEquals(_active, item) && _activeClaimGeneration == claimGeneration)
                    {
                        _active = null;
                        _activeClaimGeneration = 0;
                    }
                    CompleteSealRequestUnderLock();
                }
                if (Stopwatch.GetElapsedTime(sliceStartedAt) >= _policy.OwnerTimeBudget)
                    break;
            }
        }
        finally
        {
            lock (_admissionGate)
            {
                if (_sharedOwner is null)
                {
                    _active = null;
                    _activeClaimGeneration = 0;
                }
            }
            _drainGate.Release();
            ReleaseDrain();
        }
    }

    private bool TryTakeNext(out ZLinkSerialWorkItem item, out ulong claimGeneration)
    {
        GateOperation?.Invoke(ZLinkSerialGateOperation.ConsumerAdmissionLock);
        lock (_admissionGate)
            return TryTakeNextCore(out item, out claimGeneration);
    }

    private bool TryTakeNextCore(out ZLinkSerialWorkItem item, out ulong claimGeneration)
    {
        claimGeneration = 0;
        {
            if (_sealRequest is not null)
            {
                if (_acceptedOperations == 0)
                {
                    CompleteSealRequestUnderLock();
                }
                else
                {
                    if (!TryDequeueInfrastructureUnderLock(out item!))
                        return false;
                    claimGeneration = ClaimUnderLock(item);
                    return true;
                }
            }
            if (!TryDequeueNextUnderLock(out item!))
                return false;
            claimGeneration = ClaimUnderLock(item);
            if (item.IsAccepted)
                _acceptedOperations++;
            return true;
        }
    }

    private ulong ClaimUnderLock(ZLinkSerialWorkItem item)
    {
        ConsumerSelected?.Invoke();
        if (_sharedOwner is not null)
        {
            item.DispatchTerminal = _dispatchTerminal;
            if (item.ReservationHeld)
                _active = item;
            if (item.Lane == ZLinkSerialWorkLane.Lifecycle && item.LifecycleOwner is null)
                _activeLifecycle = item;
            return 0;
        }
        if (_nextClaimGeneration == ulong.MaxValue)
            throw new InvalidOperationException("ZLink serial claim generation is exhausted.");
        var claimGeneration = _nextClaimGeneration++;
        _active = item;
        _activeClaimGeneration = claimGeneration;
        if (item.Lane == ZLinkSerialWorkLane.Lifecycle && item.LifecycleOwner is null)
            _activeLifecycle = item;
        return claimGeneration;
    }

    private bool TryDequeueInfrastructureUnderLock(out ZLinkSerialWorkItem item)
    {
        if (_lifecycleYieldDebt && _applicationQueue.TryDequeueContinuation(out item!))
        {
            _consecutiveLifecycleTurns = 0;
            _lifecycleYieldDebt = false;
            return true;
        }
        if (_activeLifecycle is { ReadyContinuation: { } continuation })
        {
            _activeLifecycle.ReadyContinuation = null;
            item = continuation;
            RegisterSelectedLaneUnderLock(item);
            return true;
        }
        if (_activeLifecycle is null && _lifecycleQueue.TryDequeue(out item!))
        {
            RegisterSelectedLaneUnderLock(item);
            return true;
        }
        if (!_applicationQueue.TryDequeueContinuation(out item!))
            return false;
        _consecutiveLifecycleTurns = 0;
        _lifecycleYieldDebt = false;
        return true;
    }

    private bool TryDequeueNextUnderLock(out ZLinkSerialWorkItem item)
    {
        if (_applicationQueue.Head is { } head && !IsApplicationRunnable(head))
        {
            if (TryDequeueInfrastructureUnderLock(out item))
                return true;
            return _applicationQueue.TryDequeueMatching(_applicationRunnable, out item);
        }
        var lifecycleReady =
            _activeLifecycle?.ReadyContinuation is not null
            || (_activeLifecycle is null && _lifecycleQueue.Count != 0);
        var applicationReady = HasRunnableApplicationUnderLock();
        if (!lifecycleReady && !applicationReady)
        {
            item = null!;
            return false;
        }

        var chooseLifecycle = lifecycleReady && (!applicationReady || !_lifecycleYieldDebt);
        if (chooseLifecycle)
        {
            if (_activeLifecycle?.ReadyContinuation is { } continuation)
            {
                _activeLifecycle.ReadyContinuation = null;
                item = continuation;
            }
            else
                _lifecycleQueue.TryDequeue(out item!);
            RegisterSelectedLaneUnderLock(item);
            return true;
        }

        _applicationQueue.TryDequeue(out item!);
        _consecutiveLifecycleTurns = 0;
        _lifecycleYieldDebt = false;
        return true;
    }

    private bool HasRunnableApplicationUnderLock()
    {
        return _applicationQueue.Head is { } head
            && (IsApplicationRunnable(head) || _applicationQueue.Any(_applicationRunnable));
    }

    private bool IsApplicationRunnable(ZLinkSerialWorkItem item) =>
        (_sharedOwner is null || _readyItem is null || _active is null || !item.ReservationHeld)
        && (
            (!item.ReservationHeld && item.Mailbox is null)
            || _applicationQueue.Head?.Ready?.Invoke() != false
        )
        && item.Ready?.Invoke() != false
        && (
            _activeLifecycle is not { } owner
            || _applicationStartAllowed?.Invoke(owner) != false
            || !item.IsAccepted
        )
        && _applicationStartAllowed?.Invoke(item) != false;

    internal bool HasPendingAcceptedState(Func<object, bool> predicate)
    {
        if (_sharedOwner is not null && !IsInsideSharedGate)
        {
            lock (_admissionGate)
                return _relocation is null
                    && !_relocated
                    && _applicationQueue.AnyPublishedOrQueued(item =>
                        item.AcceptedState is { } state && predicate(state)
                    );
        }
        lock (_admissionGate)
        {
            if (_sharedOwner is not null)
                ImportSharedPublished();
            return _applicationQueue.Any(item =>
                item.AcceptedState is { } state && predicate(state)
            );
        }
    }

    // Spot messaging §7 step 3 under the admission gate: an accepted state
    // already waiting keeps admission open for the next incarnation; otherwise
    // application admission closes in the same decision, so a later message is
    // refused before admission.
    private bool HasPendingAcceptedStateOrCloseApplicationAdmissionOnGate(
        Func<object, bool> predicate
    )
    {
        RequireSharedGate();
        lock (_admissionGate)
        {
            if (_sharedOwner is not null)
                ImportSharedPublished();
            if (_applicationQueue.Any(item => item.AcceptedState is { } state && predicate(state)))
                return true;
            _applicationAdmissionClosed = true;
            return false;
        }
    }

    internal void VisitPendingAcceptedStateOnGate(
        ZLinkSerialExecutionQueue? successor,
        Action<object> visit
    )
    {
        RequireSharedGate();
        lock (_admissionGate)
        {
            if (_sharedOwner is not null)
                ImportSharedPublished();
            if (successor is null)
                return;
            lock (successor._admissionGate)
            {
                foreach (var item in _applicationQueue)
                {
                    if (item.AcceptedState is not { } state)
                        continue;
                    visit(state);
                    item.AcceptedState = null;
                }
            }
        }
    }

    private void RegisterSelectedLaneUnderLock(ZLinkSerialWorkItem item)
    {
        if (item.Lane != ZLinkSerialWorkLane.Lifecycle)
            throw new InvalidOperationException(
                "A lifecycle queue item was submitted to the wrong lane."
            );
        if (++_consecutiveLifecycleTurns >= _policy.LifecycleBurstLimit)
        {
            _consecutiveLifecycleTurns = 0;
            _lifecycleYieldDebt = true;
        }
    }

    private ZLinkSerialRelocationSeal SealUnderLock(
        int reservedAcceptedSequences = 0,
        IReadOnlyList<ZLinkAcceptedWorkRecord>? capturedRecords = null
    )
    {
        if (_nextRelocationSerial == ulong.MaxValue)
            throw new InvalidOperationException("ZLink relocation serial is exhausted.");
        if (reservedAcceptedSequences < 0)
            throw new ArgumentOutOfRangeException(nameof(reservedAcceptedSequences));
        if (
            reservedAcceptedSequences != 0
            && (
                _nextAcceptedSequence == ulong.MaxValue
                || checked((ulong)reservedAcceptedSequences)
                    > ulong.MaxValue - _nextAcceptedSequence
            )
        )
            throw new InvalidOperationException("ZLink accepted-work sequence is exhausted.");

        var captured = new ZLinkSerialWorkQueue();
        var held = new ZLinkSerialWorkQueue();
        var retainedApplication = new ZLinkSerialWorkQueue();
        while (_applicationQueue.TryDequeue(out var item))
        {
            if (!item.IsAccepted)
                retainedApplication.Enqueue(item);
            else
                captured.Enqueue(item);
        }
        while (retainedApplication.TryDequeue(out var item))
            _applicationQueue.Enqueue(item);

        var serial = _nextRelocationSerial++;
        var firstReservedSequence = _nextAcceptedSequence;
        _nextAcceptedSequence = checked(_nextAcceptedSequence + (ulong)reservedAcceptedSequences);
        _relocation = new ZLinkRelocationQueueState(
            serial,
            captured,
            held,
            firstReservedSequence,
            reservedAcceptedSequences
        );
        return new ZLinkSerialRelocationSeal(
            serial,
            capturedRecords
                ?? captured.Select(static item => item.CreateAcceptedRecord()).ToArray(),
            firstReservedSequence,
            reservedAcceptedSequences
        );
    }

    private void CompleteSealRequestUnderLock()
    {
        if (
            _sealRequest is null
            || _acceptedOperations != 0
            || _active is not null
            || _activeLifecycle is not null
        )
            return;
        if (_sharedOwner is not null && !Monitor.IsEntered(_admissionGate))
        {
            lock (_admissionGate)
                CompleteSealRequestUnderLock();
            return;
        }
        if (_sharedOwner is not null)
            ImportSharedPublished();
        var request = _sealRequest;
        var reserveAcceptedSequencesAtBoundary = _sealRequestReservation;
        _sealRequest = null;
        _sealRequestReservation = null;
        try
        {
            var reservedAcceptedSequences =
                reserveAcceptedSequencesAtBoundary?.Invoke()
                ?? throw new InvalidOperationException(
                    "Relocation seal reservation callback was lost."
                );
            request.TrySetResult(SealUnderLock(reservedAcceptedSequences));
        }
        catch (Exception exception)
        {
            request.TrySetException(exception);
        }
    }

    private void ReportHandlerException(Exception exception)
    {
        try
        {
            _errorSink.ReportHandlerException(exception);
        }
        catch (Exception reportException)
        {
            _taskRunner.ReportErrorSinkFailure("handler-exception-report", reportException);
        }
    }

    private void CompletePendingItem(ZLinkSerialWorkItem item)
    {
        if (_sharedOwner is not null)
        {
            if (ReferenceEquals(_active, item))
                _active = null;
            if (ReferenceEquals(_activeLifecycle, item))
                _activeLifecycle = null;
            if (item.IsAccepted)
                _acceptedOperations--;
            if (item.ReservationHeld)
                lock (_admissionGate)
                    ReleaseReservedSlotUnderLock(item.Lane);
            CompleteSharedSealRequest();
            ImportSharedPublished();
            RegisterSharedHead();
            TrySignalDrained();
            return;
        }
        Func<CancellationToken, ValueTask>? drain = null;
        lock (_admissionGate)
        {
            if (ReferenceEquals(_activeLifecycle, item))
            {
                _activeLifecycle = null;
                if (_lifecycleQueue.Count > 0)
                    drain = ReserveDrainUnderLock();
            }
            if (item.IsAccepted)
                _acceptedOperations--;
            if (item.ReservationHeld)
                ReleaseReservedSlotUnderLock(item.Lane);
            CompleteSealRequestUnderLock();
        }
        PublishDrain(drain);
        TrySignalDrained();
    }

    private void TrySignalDrained()
    {
        if (
            Volatile.Read(ref _pendingCount) == 0
            && Volatile.Read(ref _completed) != 0
            && (
                _sharedOwner is not null && _readyItem is not null
                || Volatile.Read(ref _drainScheduled) == 0
            )
        )
            _drained.TrySetResult();
    }

    private ZLinkSerialPostAdmission PostResume(
        ZLinkSerialTurn turn,
        Action<ZLinkSerialPostAdmission> resume
    )
    {
        var lifecycleOwner = turn.LifecycleOwner;
        var item = new ZLinkSerialWorkItem(
            async _ =>
            {
                turn.ResetSuspension();
                resume(ZLinkSerialPostAdmission.Accepted);
                var ownerTask = turn.OwnerTask;
                if (ownerTask is null || ownerTask.IsCompleted)
                    return;

                await Task.WhenAny(ownerTask, turn.Suspended).ConfigureAwait(false);
            },
            lane: lifecycleOwner is null
                ? ZLinkSerialWorkLane.Application
                : ZLinkSerialWorkLane.Lifecycle,
            reservationHeld: false
        );
        Func<CancellationToken, ValueTask>? drain;
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
                return ZLinkSerialPostAdmission.Closed;
            if (lifecycleOwner is null)
                CommitWorkItemUnderLock(_applicationQueue, item, ZLinkSerialWorkLane.Application);
            else
            {
                if (
                    _sharedOwner is null
                    && (
                        !ReferenceEquals(_activeLifecycle, lifecycleOwner)
                        || lifecycleOwner.ReadyContinuation is not null
                    )
                )
                    return ZLinkSerialPostAdmission.Closed;
                item.LifecycleOwner = lifecycleOwner;
                if (_sharedOwner is not null)
                {
                    item.RejectPublication = () => resume(ZLinkSerialPostAdmission.Closed);
                    CommitWorkItemUnderLock(_lifecycleQueue, item, ZLinkSerialWorkLane.Lifecycle);
                }
                else
                {
                    item.BindTerminalRelease(() => CompletePendingItem(item));
                    lifecycleOwner.ReadyContinuation = item;
                }
            }
            drain = ReserveDrainUnderLock();
        }

        PublishDrain(drain);
        return ZLinkSerialPostAdmission.Accepted;
    }

    private bool TryPostCallback(Func<CancellationToken, ValueTask> callback)
    {
        return TryPostApplication(callback, out _);
    }

    private sealed class ZLinkRelocationQueueState(
        ulong serial,
        ZLinkSerialWorkQueue captured,
        ZLinkSerialWorkQueue held,
        ulong firstReservedSequence,
        int reservedAcceptedSequences
    )
    {
        public ulong Serial { get; } = serial;

        public ZLinkSerialWorkQueue Captured { get; } = captured;

        public ulong FirstReservedSequence { get; } = firstReservedSequence;

        public int ReservedAcceptedSequences { get; } = reservedAcceptedSequences;

        public ZLinkSerialWorkQueue Held { get; } = held;

        public bool IngressFrozen { get; set; }
    }
}

internal enum ZLinkSerialGateOperation
{
    Acquire,
    Notify,
    Release,
    Scan,
    ConsumerAdmissionLock,
    ConsumerTransfer,
}

internal sealed record ZLinkSerialRelocationSeal(
    ulong Serial,
    IReadOnlyList<ZLinkAcceptedWorkRecord> Captured,
    ulong FirstReservedSequence = 0,
    int ReservedAcceptedSequences = 0
);

internal enum ZLinkAcceptedWorkAdmission
{
    Accepted = 0,
    Closed = 1,
    RelocationMoving = 2,
    Closing = 3,
}

internal static class ZLinkAcceptedWorkAdmissionErrors
{
    // The one mapping from a refused admission to its error kind
    // (spec 06-spot-address-messaging §9, 07-framework-error-model).
    internal static ZLinkFrameworkErrorKind ErrorKind(this ZLinkAcceptedWorkAdmission admission) =>
        admission switch
        {
            ZLinkAcceptedWorkAdmission.Closed => ZLinkFrameworkErrorKind.ShuttingDown,
            ZLinkAcceptedWorkAdmission.RelocationMoving => ZLinkFrameworkErrorKind.Unavailable,
            _ => ZLinkFrameworkErrorKind.Rejected,
        };
}

internal enum ZLinkSerialPostAdmission
{
    Accepted = 0,
    Closed = 1,
}
