using System.Diagnostics;

namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkSerialExecutionQueue : IAsyncDisposable
{
    private const int DefaultCapacity = 4096;
    private const int DefaultLifecycleCapacity = 256;
    private const long DefaultByteCapacity = 64L * 1024 * 1024;
    private const long DefaultLifecycleByteCapacity = 4L * 1024 * 1024;
    internal const long WorkItemFixedCostBytes = 256;
    internal const int OwnerTimeSliceMilliseconds = 10;
    internal const int LifecycleTurnLimit = 32;
    internal const int RelocationHoldMessageLimit = 1_024;
    internal const long RelocationHoldByteLimit = 16L * 1024 * 1024;
    private const int RelocationJournalRecordHeaderBytes =
        sizeof(ulong) + sizeof(int);
    private readonly int _applicationCapacity;
    private readonly int _lifecycleCapacity;
    private readonly long _applicationByteCapacity;
    private readonly long _lifecycleByteCapacity;
    private readonly object _admissionGate = new();
    private readonly object _disposeGate = new();

    private readonly TaskCompletionSource _drained =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private readonly SemaphoreSlim _drainGate = new(1, 1);
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationToken _executionToken;
    private readonly Queue<ZLinkSerialWorkItem> _applicationQueue = new();
    private readonly Queue<ZLinkSerialWorkItem> _lifecycleQueue = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private ZLinkSerialWorkItem? _active;
    private int _completed;
    private int _disposed;
    private Task? _disposeTask;
    private int _drainScheduled;
    private int _pendingCount;
    private int _applicationPendingCount;
    private int _lifecyclePendingCount;
    private long _applicationPendingBytes;
    private long _lifecyclePendingBytes;
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

    public ZLinkSerialExecutionQueue(
        ZLinkRuntimeTaskRunner taskRunner,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken executionToken,
        int capacity = DefaultCapacity,
        long applicationByteCapacity = DefaultByteCapacity,
        long lifecycleByteCapacity = DefaultLifecycleByteCapacity)
    {
        _taskRunner = taskRunner;
        _errorSink = errorSink;
        _executionToken = executionToken;
        _applicationCapacity = capacity > 0
            ? capacity
            : throw new ArgumentOutOfRangeException(nameof(capacity));
        _lifecycleCapacity = DefaultLifecycleCapacity;
        if (applicationByteCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(applicationByteCapacity));
        if (lifecycleByteCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(lifecycleByteCapacity));
        _applicationByteCapacity = applicationByteCapacity;
        _lifecycleByteCapacity = lifecycleByteCapacity;
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
                start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
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
        catch (OperationCanceledException)
        {
        }
        catch (ObjectDisposedException)
        {
        }

        _drainGate.Dispose();
    }

    public void Complete()
    {
        TaskCompletionSource<ZLinkSerialRelocationSeal>? pendingSeal;
        lock (_admissionGate)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0) return;
            pendingSeal = _sealRequest;
            _sealRequest = null;
            _sealRequestReservation = null;
            AbortRelocationUnderLock();
            if (_applicationQueue.Count > 0 || _lifecycleQueue.Count > 0)
                ScheduleDrain();
        }
        pendingSeal?.TrySetException(
            new InvalidOperationException(
                "ZLink serial execution queue closed before relocation seal completed."));
        TrySignalDrained();
    }

    public ValueTask<ZLinkSerialWorkItem> PostAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var admission = TryPostApplicationWithAdmission(callback, out var item);
        if (admission != ZLinkSerialPostAdmission.Accepted)
            throw CreateAdmissionException(admission, "execution");
        return ValueTask.FromResult(item);
    }

    public bool TryPost(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
    {
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0
                || !TryReserveSlot(
                    ZLinkSerialWorkLane.Application,
                    WorkItemFixedCostBytes,
                    _applicationCapacity,
                    _applicationByteCapacity))
            {
                item = null!;
                return false;
            }

            item = new ZLinkSerialWorkItem(
                callback,
                lane: ZLinkSerialWorkLane.Application,
                accountingBytes: WorkItemFixedCostBytes);
            _applicationQueue.Enqueue(item);
            ScheduleDrain();
            return true;
        }
    }

    public bool TryPostApplication(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
        => TryPostApplicationWithAdmission(callback, out item)
            == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission TryPostApplicationWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
    {
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return ZLinkSerialPostAdmission.Closed;
            }
            if (!TryReserveSlot(
                    ZLinkSerialWorkLane.Application,
                    WorkItemFixedCostBytes,
                    _applicationCapacity,
                    _applicationByteCapacity))
            {
                item = null!;
                return ZLinkSerialPostAdmission.QueueFull;
            }

            item = new ZLinkSerialWorkItem(
                callback,
                lane: ZLinkSerialWorkLane.Application,
                accountingBytes: WorkItemFixedCostBytes);
            _applicationQueue.Enqueue(item);
            ScheduleDrain();
            return ZLinkSerialPostAdmission.Accepted;
        }
    }

    public bool TryPostNext(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
        => TryPostNextWithAdmission(callback, out item)
            == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission TryPostNextWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
    {
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return ZLinkSerialPostAdmission.Closed;
            }
            if (!TryReserveSlot(
                    ZLinkSerialWorkLane.Lifecycle,
                    WorkItemFixedCostBytes,
                    _lifecycleCapacity,
                    _lifecycleByteCapacity))
            {
                item = null!;
                return ZLinkSerialPostAdmission.QueueFull;
            }

            item = new ZLinkSerialWorkItem(
                callback,
                lane: ZLinkSerialWorkLane.Lifecycle,
                accountingBytes: WorkItemFixedCostBytes);
            _lifecycleQueue.Enqueue(item);
            ScheduleDrain();
            return ZLinkSerialPostAdmission.Accepted;
        }
    }

    public ZLinkAcceptedWorkAdmission TryPostAccepted(
        ReadOnlyMemory<byte> payload,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        out ZLinkSerialWorkItem item) =>
        TryPostAccepted(
            payload,
            callback,
            relocationRelease,
            previousOwnerMessageFollow: false,
            out item);

    public ZLinkAcceptedWorkAdmission TryPostAccepted(
        ReadOnlyMemory<byte> payload,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out ZLinkSerialWorkItem item)
        => TryPostAcceptedCore(
            payload.Length,
            null,
            payload,
            callback,
            relocationRelease,
            previousOwnerMessageFollow,
            out item);

    internal ZLinkAcceptedWorkAdmission TryPostAccepted(
        int payloadLength,
        Func<ReadOnlyMemory<byte>> payloadFactory,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out ZLinkSerialWorkItem item)
    {
        ArgumentNullException.ThrowIfNull(payloadFactory);
        return TryPostAcceptedCore(
            payloadLength,
            payloadFactory,
            default,
            callback,
            relocationRelease,
            previousOwnerMessageFollow,
            out item);
    }

    private ZLinkAcceptedWorkAdmission TryPostAcceptedCore(
        int payloadLength,
        Func<ReadOnlyMemory<byte>>? payloadFactory,
        ReadOnlyMemory<byte> payload,
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        out ZLinkSerialWorkItem item)
    {
        ArgumentNullException.ThrowIfNull(callback);
        ArgumentNullException.ThrowIfNull(relocationRelease);
        if (payloadLength < 0)
            throw new ArgumentOutOfRangeException(nameof(payloadLength));

        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.Closed;
            }
            if (_relocated
                || _relocation?.IngressFrozen == true
                || (_relocation is { } relocation
                    && (relocation.Held.Count
                            >= RelocationHoldMessageLimit
                        || EncodedRelocationRecordBytes(payloadLength)
                            > RelocationHoldByteLimit
                              - relocation.HeldBytes)))
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.RelocationMoving;
            }
            var accountingBytes = checked(
                WorkItemFixedCostBytes + (long)payloadLength);
            if (!TryReserveSlot(
                    ZLinkSerialWorkLane.Application,
                    accountingBytes,
                    _applicationCapacity,
                    _applicationByteCapacity))
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.QueueFull;
            }
            if (_nextAcceptedSequence == ulong.MaxValue)
            {
                ReleaseReservedSlotUnderLock(
                    ZLinkSerialWorkLane.Application,
                    accountingBytes);
                throw new InvalidOperationException(
                    "ZLink accepted-work sequence is exhausted.");
            }

            try
            {
                var acceptedSequence = _nextAcceptedSequence++;
                item = new ZLinkSerialWorkItem(
                    callback,
                    relocationRelease,
                    previousOwnerMessageFollow,
                    ZLinkSerialWorkLane.Application,
                    accountingBytes,
                    acceptedSequence,
                    payload,
                    payloadFactory);
            }
            catch
            {
                ReleaseReservedSlotUnderLock(
                    ZLinkSerialWorkLane.Application,
                    accountingBytes);
                throw;
            }
            if (_relocation is null)
            {
                _applicationQueue.Enqueue(item);
                ScheduleDrain();
            }
            else
            {
                _relocation.Held.Enqueue(item);
                _relocation.HeldBytes = checked(
                    _relocation.HeldBytes
                    + EncodedRelocationRecordBytes(payloadLength));
            }
            return ZLinkAcceptedWorkAdmission.Accepted;
        }
    }

    private static long EncodedRelocationRecordBytes(int payloadLength) =>
        checked(RelocationJournalRecordHeaderBytes + (long)payloadLength);

    public bool TryPostFinal(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
    {
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0
                || !TryReserveSlot(
                    ZLinkSerialWorkLane.Lifecycle,
                    WorkItemFixedCostBytes,
                    _lifecycleCapacity,
                    _lifecycleByteCapacity))
            {
                item = null!;
                return false;
            }

            AbortRelocationUnderLock();
            Volatile.Write(ref _completed, 1);
            item = new ZLinkSerialWorkItem(
                callback,
                lane: ZLinkSerialWorkLane.Lifecycle,
                accountingBytes: WorkItemFixedCostBytes);
            _lifecycleQueue.Enqueue(item);
            ScheduleDrain();
            return true;
        }
    }

    public bool TrySealRelocation(out ZLinkSerialRelocationSeal seal)
    {
        lock (_admissionGate)
        {
            if (_relocated
                || _relocation is not null
                || _active is not null
                || _acceptedOperations != 0
                || _sealRequest is not null
                || Volatile.Read(ref _completed) != 0)
            {
                seal = null!;
                return false;
            }
            seal = SealUnderLock();
            return true;
        }
    }

    internal bool TrySealRelocation(
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        out ZLinkSerialRelocationSeal seal)
        => TrySealRelocation(0, admit, out seal, out _);

    internal bool TrySealRelocation(
        int reservedAcceptedSequences,
        Func<IReadOnlyList<ZLinkAcceptedWorkRecord>, bool> admit,
        out ZLinkSerialRelocationSeal seal,
        out ulong firstReservedSequence)
    {
        ArgumentNullException.ThrowIfNull(admit);
        if (reservedAcceptedSequences < 0)
            throw new ArgumentOutOfRangeException(nameof(reservedAcceptedSequences));
        lock (_admissionGate)
        {
            if (_relocated
                || _relocation is not null
                || _active is not null
                || _acceptedOperations != 0
                || _sealRequest is not null
                || Volatile.Read(ref _completed) != 0)
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
        CancellationToken cancellationToken) =>
        SealRelocationAsync(
            static () => 0,
            cancellationToken);

    internal ValueTask<ZLinkSerialRelocationSeal> SealRelocationAsync(
        Func<int> reserveAcceptedSequencesAtBoundary,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ArgumentNullException.ThrowIfNull(
            reserveAcceptedSequencesAtBoundary);
        TaskCompletionSource<ZLinkSerialRelocationSeal> request;
        lock (_admissionGate)
        {
            if (_relocated)
                throw new InvalidOperationException(
                    "ZLink serial queue owner has already relocated.");
            if (_relocation is not null)
                throw new InvalidOperationException(
                    "ZLink serial queue owner is already sealed for relocation.");
            if (Volatile.Read(ref _completed) != 0)
                throw new InvalidOperationException(
                    "ZLink serial execution queue is closed.");
            if (_sealRequest is not null)
                throw new InvalidOperationException(
                    "ZLink serial execution queue already has a pending relocation seal.");

            request = new TaskCompletionSource<ZLinkSerialRelocationSeal>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _sealRequest = request;
            _sealRequestReservation =
                reserveAcceptedSequencesAtBoundary;
            if (_active is null && _acceptedOperations == 0)
                CompleteSealRequestUnderLock();
            else
                ScheduleDrain();
        }
        return AwaitSealRequestAsync(
            request,
            cancellationToken);
    }

    private async ValueTask<ZLinkSerialRelocationSeal> AwaitSealRequestAsync(
        TaskCompletionSource<ZLinkSerialRelocationSeal> request,
        CancellationToken cancellationToken)
    {
        try
        {
            return await request.Task.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            lock (_admissionGate)
            {
                if (ReferenceEquals(_sealRequest, request))
                {
                    _sealRequest = null;
                    _sealRequestReservation = null;
                    ScheduleDrain();
                }
                else if (request.Task.IsCompletedSuccessfully
                         && Matches(request.Task.Result))
                {
                    AbortRelocationUnderLock();
                    ScheduleDrain();
                }
            }
            throw;
        }
    }

    public bool TryAbortRelocation(ZLinkSerialRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(seal);
        lock (_admissionGate)
        {
            if (!Matches(seal)) return false;
            AbortRelocationUnderLock();
            ScheduleDrain();
            return true;
        }
    }

    public bool TryOpenRelocationAfterMessageFollow(
        ZLinkSerialRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(seal);
        lock (_admissionGate)
        {
            if (!Matches(seal))
                return false;
            var relocation = _relocation!;
            while (relocation.Captured.TryDequeue(out var item))
                _applicationQueue.Enqueue(item);
            var direct = new Queue<ZLinkSerialWorkItem>();
            while (relocation.Held.TryDequeue(out var item))
            {
                if (item.PreviousOwnerMessageFollow)
                    _applicationQueue.Enqueue(item);
                else
                    direct.Enqueue(item);
            }
            while (direct.TryDequeue(out var item))
                _applicationQueue.Enqueue(item);
            _relocation = null;
            ScheduleDrain();
            return true;
        }
    }

    private void AbortRelocationUnderLock()
    {
        if (_relocation is null) return;
        while (_relocation.Captured.TryDequeue(out var item))
            _applicationQueue.Enqueue(item);
        while (_relocation.Held.TryDequeue(out var item))
            _applicationQueue.Enqueue(item);
        _relocation = null;
    }

    public bool TryCommitRelocation(
        ZLinkSerialRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ZLinkSerialWorkItem[] released;
        lock (_admissionGate)
        {
            if (!Matches(seal))
            {
                held = [];
                return false;
            }

            held = _relocation!.Held
                .Select(static item => item.CreateAcceptedRecord())
                .ToArray();
            released = _relocation.Captured
                .Concat(_relocation.Held)
                .ToArray();
            _relocation.Captured.Clear();
            _relocation.Held.Clear();
            _relocation = null;
            _relocated = true;
        }

        foreach (var item in released)
        {
            item.ReleaseForRelocation(ReportHandlerException);
            CompletePendingItem(item);
        }
        return true;
    }

    public bool TryFreezeRelocationIngress(
        ZLinkSerialRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held)
    {
        ArgumentNullException.ThrowIfNull(seal);
        lock (_admissionGate)
        {
            if (!Matches(seal))
            {
                held = [];
                return false;
            }
            _relocation!.IngressFrozen = true;
            held = _relocation.Held
                .Select(static item => item.CreateAcceptedRecord())
                .ToArray();
            return true;
        }
    }

    private bool Matches(ZLinkSerialRelocationSeal seal)
    {
        return _relocation is not null
               && _relocation.Serial == seal.Serial;
    }

    private bool TryReserveSlot(
        ZLinkSerialWorkLane lane,
        long accountingBytes,
        int countLimit,
        long byteLimit)
    {
        if (accountingBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(accountingBytes));

        ref var pendingCount = ref PendingCount(lane);
        ref var pendingBytes = ref PendingBytes(lane);
        if (pendingCount >= countLimit
            || accountingBytes > byteLimit - pendingBytes)
            return false;

        pendingCount++;
        pendingBytes += accountingBytes;
        _pendingCount++;
        return true;
    }

    private void ReleaseReservedSlotUnderLock(
        ZLinkSerialWorkLane lane,
        long accountingBytes)
    {
        ref var pendingCount = ref PendingCount(lane);
        ref var pendingBytes = ref PendingBytes(lane);
        pendingCount--;
        pendingBytes -= accountingBytes;
        _pendingCount--;
    }

    private ref int PendingCount(ZLinkSerialWorkLane lane)
    {
        if (lane == ZLinkSerialWorkLane.Application)
            return ref _applicationPendingCount;
        return ref _lifecyclePendingCount;
    }

    private ref long PendingBytes(ZLinkSerialWorkLane lane)
    {
        if (lane == ZLinkSerialWorkLane.Application)
            return ref _applicationPendingBytes;
        return ref _lifecyclePendingBytes;
    }

    public async ValueTask RunAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken)
    {
        var item = await PostAsync(callback, cancellationToken).ConfigureAwait(false);
        await item.Completion.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    internal async ValueTask RunLifecycleAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var admission = TryPostNextWithAdmission(callback, out var item);
        if (admission != ZLinkSerialPostAdmission.Accepted)
            throw CreateAdmissionException(admission, "lifecycle");
        await item.Completion.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private static ZLinkFrameworkException CreateAdmissionException(
        ZLinkSerialPostAdmission admission,
        string lane)
    {
        var closed = admission == ZLinkSerialPostAdmission.Closed;
        return new ZLinkFrameworkException(
            closed
                ? ZLinkFrameworkErrorKind.ShuttingDown
                : ZLinkFrameworkErrorKind.CapacityExceeded,
            closed
                ? $"The serial {lane} queue is closed."
                : $"The serial {lane} queue is at capacity.");
    }

    private void ScheduleDrain()
    {
        if (Interlocked.Exchange(ref _drainScheduled, 1) != 0)
            return;

        if (!_taskRunner.TryRunDetached("serial-queue-drain", DrainAsync))
            _ = DrainAsync(CancellationToken.None);
    }

    private async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        if (!await _drainGate.WaitAsync(0, CancellationToken.None).ConfigureAwait(false))
        {
            Volatile.Write(ref _drainScheduled, 0);
            if (HasQueuedWork())
                ScheduleDrain();
            else
                TrySignalDrained();

            return;
        }

        try
        {
            var sliceStartedAt = Stopwatch.GetTimestamp();
            while (TryTakeNext(out var item, out var claimGeneration))
            {
                var turn = new ZLinkSerialTurn(
                    PostResume,
                    TryPostCallback,
                    ReportHandlerException,
                    _executionToken);
                var result = await item.InvokeAsync(
                    ReportHandlerException,
                    _executionToken,
                    turn).ConfigureAwait(false);
                lock (_admissionGate)
                {
                    if (ReferenceEquals(_active, item)
                        && _activeClaimGeneration == claimGeneration)
                    {
                        _active = null;
                        _activeClaimGeneration = 0;
                    }
                }
                if (result == ZLinkSerialWorkItemResult.Completed)
                    CompletePendingItem(item);
                else
                    _ = item.Completion.ContinueWith(
                        static (task, state) =>
                        {
                            var completion = ((ZLinkSerialExecutionQueue Queue, ZLinkSerialWorkItem Item))state!;
                            completion.Queue.CompletePendingItem(completion.Item);
                        },
                        (this, item),
                        CancellationToken.None,
                        TaskContinuationOptions.ExecuteSynchronously,
                        TaskScheduler.Default);

                if (ElapsedMilliseconds(sliceStartedAt)
                    >= OwnerTimeSliceMilliseconds)
                    break;
            }
        }
        finally
        {
            lock (_admissionGate)
            {
                _active = null;
                _activeClaimGeneration = 0;
            }
            _drainGate.Release();
        }

        Volatile.Write(ref _drainScheduled, 0);
        if (HasQueuedWork())
            ScheduleDrain();
        else
            TrySignalDrained();
    }

    private bool TryTakeNext(
        out ZLinkSerialWorkItem item,
        out ulong claimGeneration)
    {
        claimGeneration = 0;
        lock (_admissionGate)
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
        if (_nextClaimGeneration == ulong.MaxValue)
            throw new InvalidOperationException(
                "ZLink serial claim generation is exhausted.");
        var claimGeneration = _nextClaimGeneration++;
        _active = item;
        _activeClaimGeneration = claimGeneration;
        return claimGeneration;
    }

    private bool TryDequeueInfrastructureUnderLock(out ZLinkSerialWorkItem item)
    {
        if (!_lifecycleQueue.TryDequeue(out item!))
            return false;
        RegisterSelectedLaneUnderLock(item);
        return true;
    }

    private bool TryDequeueNextUnderLock(out ZLinkSerialWorkItem item)
    {
        var lifecycleReady = _lifecycleQueue.Count != 0;
        var applicationReady = _applicationQueue.Count != 0;
        if (!lifecycleReady && !applicationReady)
        {
            item = null!;
            return false;
        }

        var chooseLifecycle = lifecycleReady
                              && (!applicationReady || !_lifecycleYieldDebt);
        if (chooseLifecycle)
        {
            _lifecycleQueue.TryDequeue(out item!);
            RegisterSelectedLaneUnderLock(item);
            return true;
        }

        _applicationQueue.TryDequeue(out item!);
        _consecutiveLifecycleTurns = 0;
        _lifecycleYieldDebt = false;
        return true;
    }

    private void RegisterSelectedLaneUnderLock(ZLinkSerialWorkItem item)
    {
        if (item.Lane != ZLinkSerialWorkLane.Lifecycle)
            throw new InvalidOperationException(
                "A lifecycle queue item was submitted to the wrong lane.");
        if (++_consecutiveLifecycleTurns >= LifecycleTurnLimit)
        {
            _consecutiveLifecycleTurns = 0;
            _lifecycleYieldDebt = true;
        }
    }

    private static long ElapsedMilliseconds(long startedAt)
    {
        var elapsedTicks = Stopwatch.GetTimestamp() - startedAt;
        if (elapsedTicks <= 0)
            return 0;
        var threshold = Stopwatch.Frequency
                        * (long)OwnerTimeSliceMilliseconds
                        / 1000L;
        return elapsedTicks >= threshold
            ? OwnerTimeSliceMilliseconds
            : elapsedTicks * 1000L / Stopwatch.Frequency;
    }

    private ZLinkSerialRelocationSeal SealUnderLock(
        int reservedAcceptedSequences = 0,
        IReadOnlyList<ZLinkAcceptedWorkRecord>? capturedRecords = null)
    {
        if (_nextRelocationSerial == ulong.MaxValue)
            throw new InvalidOperationException(
                "ZLink relocation serial is exhausted.");
        if (reservedAcceptedSequences < 0)
            throw new ArgumentOutOfRangeException(
                nameof(reservedAcceptedSequences));
        if (reservedAcceptedSequences != 0
            && (_nextAcceptedSequence == ulong.MaxValue
                || checked((ulong)reservedAcceptedSequences)
                   > ulong.MaxValue - _nextAcceptedSequence))
            throw new InvalidOperationException(
                "ZLink accepted-work sequence is exhausted.");

        var captured = new Queue<ZLinkSerialWorkItem>();
        var retainedApplication = new Queue<ZLinkSerialWorkItem>();
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
        _nextAcceptedSequence = checked(
            _nextAcceptedSequence + (ulong)reservedAcceptedSequences);
        _relocation = new ZLinkRelocationQueueState(
            serial,
            captured,
            firstReservedSequence,
            reservedAcceptedSequences);
        return new ZLinkSerialRelocationSeal(
            serial,
            capturedRecords
                ?? captured
                    .Select(static item => item.CreateAcceptedRecord())
                    .ToArray(),
            firstReservedSequence,
            reservedAcceptedSequences);
    }

    private void CompleteSealRequestUnderLock()
    {
        if (_sealRequest is null || _acceptedOperations != 0 || _active is not null)
            return;
        var request = _sealRequest;
        var reserveAcceptedSequencesAtBoundary =
            _sealRequestReservation;
        _sealRequest = null;
        _sealRequestReservation = null;
        try
        {
            var reservedAcceptedSequences =
                reserveAcceptedSequencesAtBoundary?.Invoke()
                ?? throw new InvalidOperationException(
                    "Relocation seal reservation callback was lost.");
            request.TrySetResult(SealUnderLock(reservedAcceptedSequences));
        }
        catch (Exception exception)
        {
            request.TrySetException(exception);
        }
    }

    private bool HasQueuedWork()
    {
        lock (_admissionGate)
            return _applicationQueue.Count > 0 || _lifecycleQueue.Count > 0;
    }

    private void ReportHandlerException(Exception exception)
    {
        try
        {
            _errorSink.ReportHandlerException(exception);
        }
        catch (Exception reportException)
        {
            _taskRunner.ReportErrorSinkFailure(
                "handler-exception-report",
                reportException);
        }
    }

    private void CompletePendingItem(ZLinkSerialWorkItem item)
    {
        lock (_admissionGate)
        {
            if (item.IsAccepted)
                _acceptedOperations--;
            ReleaseReservedSlotUnderLock(item.Lane, item.AccountingBytes);
            CompleteSealRequestUnderLock();
        }
        TrySignalDrained();
    }

    private void TrySignalDrained()
    {
        if (Volatile.Read(ref _pendingCount) == 0
            && Volatile.Read(ref _completed) != 0
            && Volatile.Read(ref _drainScheduled) == 0)
            _drained.TrySetResult();
    }

    private ZLinkSerialPostAdmission PostResume(
        ZLinkSerialTurn turn,
        Action resume)
    {
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
                return ZLinkSerialPostAdmission.Closed;
            if (!TryReserveSlot(
                    ZLinkSerialWorkLane.Application,
                    WorkItemFixedCostBytes,
                    _applicationCapacity,
                    _applicationByteCapacity))
                return ZLinkSerialPostAdmission.QueueFull;

            var item = new ZLinkSerialWorkItem(async _ =>
            {
                turn.ResetSuspension();
                resume();
                var ownerTask = turn.OwnerTask;
                if (ownerTask is null || ownerTask.IsCompleted) return;

                await Task.WhenAny(ownerTask, turn.Suspended).ConfigureAwait(false);
            });
            _applicationQueue.Enqueue(item);
        }

        ScheduleDrain();
        return ZLinkSerialPostAdmission.Accepted;
    }

    private bool TryPostCallback(Func<CancellationToken, ValueTask> callback)
    {
        return TryPostApplication(callback, out _);
    }

    private sealed class ZLinkRelocationQueueState(
        ulong serial,
        Queue<ZLinkSerialWorkItem> captured,
        ulong firstReservedSequence,
        int reservedAcceptedSequences)
    {
        public ulong Serial { get; } = serial;

        public Queue<ZLinkSerialWorkItem> Captured { get; } = captured;

        public ulong FirstReservedSequence { get; } =
            firstReservedSequence;

        public int ReservedAcceptedSequences { get; } =
            reservedAcceptedSequences;

        public Queue<ZLinkSerialWorkItem> Held { get; } = new();

        public long HeldBytes { get; set; }

        public bool IngressFrozen { get; set; }
    }
}

internal sealed record ZLinkSerialRelocationSeal(
    ulong Serial,
    IReadOnlyList<ZLinkAcceptedWorkRecord> Captured,
    ulong FirstReservedSequence = 0,
    int ReservedAcceptedSequences = 0);

internal enum ZLinkAcceptedWorkAdmission
{
    Accepted = 0,
    Closed = 1,
    QueueFull = 2,
    RelocationMoving = 3
}

internal enum ZLinkSerialPostAdmission
{
    Accepted = 0,
    QueueFull = 1,
    Closed = 2
}
