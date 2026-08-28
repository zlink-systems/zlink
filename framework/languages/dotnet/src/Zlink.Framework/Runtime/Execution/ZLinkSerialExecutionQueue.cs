using System.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkSerialExecutionQueue : IAsyncDisposable
{
    private const int RelocationJournalRecordHeaderBytes =
        sizeof(ulong) + sizeof(int);
    private readonly object _admissionGate = new();
    private readonly object _disposeGate = new();

    private readonly TaskCompletionSource _drained =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private TaskCompletionSource _applicationDrained = CompletedSignal();

    private readonly SemaphoreSlim _drainGate = new(1, 1);
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationToken _executionToken;
    private readonly ZLinkExecutionLanePolicy _policy;
    // Instance method groups convert to a fresh delegate at every use site;
    // these run once per drained work item, so cache them.
    private readonly Func<ZLinkSerialTurn, Action, ZLinkSerialPostAdmission> _postResume;
    private readonly Func<Func<CancellationToken, ValueTask>, bool> _tryPostCallback;
    private readonly Action<Exception> _reportHandlerException;
    private readonly ZLinkSerialWorkQueue _applicationQueue = new();
    private readonly ZLinkSerialWorkQueue _lifecycleQueue = new();
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private ZLinkSerialWorkItem? _active;
    private int _completed;
    private bool _applicationAdmissionClosed;
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

    internal int ApplicationPendingCount
    {
        get
        {
            lock (_admissionGate) return _applicationPendingCount;
        }
    }

    internal int LifecyclePendingCount
    {
        get
        {
            lock (_admissionGate) return _lifecyclePendingCount;
        }
    }

    internal long ApplicationPendingBytes
    {
        get
        {
            lock (_admissionGate) return _applicationPendingBytes;
        }
    }

    internal long LifecyclePendingBytes
    {
        get
        {
            lock (_admissionGate) return _lifecyclePendingBytes;
        }
    }

    internal Task ApplicationDrained
    {
        get
        {
            lock (_admissionGate) return _applicationDrained.Task;
        }
    }

    public ZLinkSerialExecutionQueue(
        ZLinkRuntimeTaskRunner taskRunner,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken executionToken)
        : this(
            taskRunner,
            errorSink,
            executionToken,
            ZLinkExecutionLanePolicy.Default)
    {
    }

    public ZLinkSerialExecutionQueue(
        ZLinkRuntimeTaskRunner taskRunner,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken executionToken,
        ZLinkExecutionLanePolicy policy)
    {
        ArgumentNullException.ThrowIfNull(taskRunner);
        ArgumentNullException.ThrowIfNull(errorSink);
        ArgumentNullException.ThrowIfNull(policy);
        _taskRunner = taskRunner;
        _errorSink = errorSink;
        _executionToken = executionToken;
        _policy = policy;
        _postResume = PostResume;
        _tryPostCallback = TryPostCallback;
        _reportHandlerException = ReportHandlerException;
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

    internal void CloseApplicationAdmission()
    {
        lock (_admissionGate)
            _applicationAdmissionClosed = true;
    }

    public ValueTask<ZLinkSerialWorkItem> PostAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken)
        => PostAsync(
            callback,
            payloadBytes: 0,
            metadataBytes: 0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            cancellationToken);

    internal ValueTask<ZLinkSerialWorkItem> PostAsync(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var admission = TryPostApplicationWithAdmission(
            callback,
            payloadBytes,
            metadataBytes,
            transferred,
            out var item);
        if (admission != ZLinkSerialPostAdmission.Accepted)
            throw CreateAdmissionException("execution", admission);
        return ValueTask.FromResult(item);
    }

    public bool TryPost(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
        => TryPostApplicationWithAdmission(callback, out item)
            == ZLinkSerialPostAdmission.Accepted;

    public bool TryPostApplication(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
        => TryPostApplicationWithAdmission(callback, out item)
            == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission TryPostApplicationWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
        => TryPostApplicationWithAdmission(
            callback,
            payloadBytes: 0,
            metadataBytes: 0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            out item);

    internal ZLinkSerialPostAdmission TryPostApplicationWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        out ZLinkSerialWorkItem item)
    {
        ArgumentNullException.ThrowIfNull(callback);
        if (payloadBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(payloadBytes));
        if (metadataBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(metadataBytes));
        if (!TryGetByteCost(payloadBytes, metadataBytes, out var byteCost))
        {
            item = null!;
            return ZLinkSerialPostAdmission.CapacityExceeded;
        }
        var candidate = new ZLinkSerialWorkItem(
            callback,
            lane: ZLinkSerialWorkLane.Application,
            byteCost: byteCost);
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0
                || _applicationAdmissionClosed)
            {
                item = null!;
                return ZLinkSerialPostAdmission.Closed;
            }
            if (!CommitWorkItemUnderLock(
                _applicationQueue,
                candidate,
                ZLinkSerialWorkLane.Application,
                bypassCapacity: transferred))
            {
                item = null!;
                return ZLinkSerialPostAdmission.CapacityExceeded;
            }
            item = candidate;
        }

        if (transferred)
            _ = ZLinkApplicationJobQueueInvocation.TryTransferOwnerReservation();
        ScheduleDrain();
        return ZLinkSerialPostAdmission.Accepted;
    }

    public bool TryPostNext(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
        => TryPostNextWithAdmission(callback, out item)
            == ZLinkSerialPostAdmission.Accepted;

    internal ZLinkSerialPostAdmission TryPostNextWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item) =>
        TryPostNextWithAdmission(
            callback,
            payloadBytes: 0,
            metadataBytes: 0,
            ZLinkApplicationJobQueueInvocation.HasTransferableOwnerReservation(),
            out item);

    internal ZLinkSerialPostAdmission TryPostNextWithAdmission(
        Func<CancellationToken, ValueTask> callback,
        long payloadBytes,
        long metadataBytes,
        bool transferred,
        out ZLinkSerialWorkItem item)
    {
        ArgumentNullException.ThrowIfNull(callback);
        if (payloadBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(payloadBytes));
        if (metadataBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(metadataBytes));
        if (!TryGetByteCost(payloadBytes, metadataBytes, out var byteCost))
        {
            item = null!;
            return ZLinkSerialPostAdmission.CapacityExceeded;
        }
        var candidate = new ZLinkSerialWorkItem(
            callback,
            lane: ZLinkSerialWorkLane.Lifecycle,
            byteCost: byteCost);
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return ZLinkSerialPostAdmission.Closed;
            }
            if (!CommitWorkItemUnderLock(
                _lifecycleQueue,
                candidate,
                ZLinkSerialWorkLane.Lifecycle,
                bypassCapacity: transferred))
            {
                item = null!;
                return ZLinkSerialPostAdmission.CapacityExceeded;
            }
            item = candidate;
        }

        if (transferred)
            _ = ZLinkApplicationJobQueueInvocation.TryTransferOwnerReservation();
        ScheduleDrain();
        return ZLinkSerialPostAdmission.Accepted;
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
        EnsureRelocationRecordLength(payloadLength);
        var scheduleDrain = false;

        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0
                || _applicationAdmissionClosed)
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.Closed;
            }
            if (_relocated
                || _relocation?.IngressFrozen == true)
            {
                item = null!;
                return ZLinkAcceptedWorkAdmission.RelocationMoving;
            }
            if (!TryCommitAcceptedWorkUnderLock(
                callback,
                relocationRelease,
                previousOwnerMessageFollow,
                payloadLength,
                payload,
                payloadFactory,
                out item,
                out scheduleDrain))
                return ZLinkAcceptedWorkAdmission.CapacityExceeded;
        }

        _ = ZLinkApplicationJobQueueInvocation.TryTransferOwnerReservation();
        if (scheduleDrain) ScheduleDrain();
        return ZLinkAcceptedWorkAdmission.Accepted;
    }

    private static void EnsureRelocationRecordLength(int payloadLength) =>
        _ = checked(RelocationJournalRecordHeaderBytes + (long)payloadLength);

    public bool TryPostFinal(
        Func<CancellationToken, ValueTask> callback,
        out ZLinkSerialWorkItem item)
    {
        ArgumentNullException.ThrowIfNull(callback);
        // The terminal closes admission, but it must remain behind every
        // application turn that was already accepted. Placing it in the
        // lifecycle lane would let lifecycle priority overtake and dispose
        // those accepted turns.
        var candidate = new ZLinkSerialWorkItem(
            callback,
            lane: ZLinkSerialWorkLane.Application,
            byteCost: _policy.FixedWorkByteCost);
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
            {
                item = null!;
                return false;
            }

            AbortRelocationUnderLock();
            CommitWorkItemUnderLock(
                _applicationQueue,
                candidate,
                ZLinkSerialWorkLane.Application,
                bypassCapacity: true);
            Volatile.Write(ref _completed, 1);
            item = candidate;
        }
        ScheduleDrain();
        return true;
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
            var direct = new ZLinkSerialWorkQueue();
            while (relocation.Captured.TryDequeue(out var item))
                _applicationQueue.Enqueue(item);
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
            item.ReleaseForRelocation(ReportHandlerException);
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

    private bool CommitWorkItemUnderLock(
        ZLinkSerialWorkQueue destination,
        ZLinkSerialWorkItem item,
        ZLinkSerialWorkLane lane,
        bool bypassCapacity)
    {
        if (!item.ReservationHeld)
        {
            item.BindTerminalRelease(() => CompletePendingItem(item));
            destination.Enqueue(item);
            return true;
        }
        ref var pendingCount = ref PendingCount(lane);
        ref var pendingBytes = ref PendingBytes(lane);
        if (!bypassCapacity && !CanReserveUnderLock(lane, item.ByteCost))
            return false;
        if (pendingCount == int.MaxValue
            || _pendingCount == int.MaxValue
            || item.ByteCost > long.MaxValue - pendingBytes)
            return false;

        // This queue owns ordering only. The queued callback retains the
        // actual Core receive owner until its terminal path disposes it.
        var applicationDrained = NewApplicationDrainedSignalUnderLock(lane);
        item.BindTerminalRelease(() => CompletePendingItem(item));
        destination.Enqueue(item);
        CommitReservationUnderLock(lane, item.ByteCost, applicationDrained);
        return true;
    }

    private bool TryCommitAcceptedWorkUnderLock(
        Func<CancellationToken, ValueTask> callback,
        Action relocationRelease,
        bool previousOwnerMessageFollow,
        int payloadLength,
        ReadOnlyMemory<byte> payload,
        Func<ReadOnlyMemory<byte>>? payloadFactory,
        out ZLinkSerialWorkItem item,
        out bool scheduleDrain)
    {
        if (_nextAcceptedSequence == ulong.MaxValue)
            throw new InvalidOperationException(
                "ZLink accepted-work sequence is exhausted.");

        var acceptedSequence = _nextAcceptedSequence;
        var destination = _relocation?.Held ?? _applicationQueue;
        if (!TryGetByteCost(payloadLength, 0, out var byteCost))
        {
            item = null!;
            scheduleDrain = false;
            return false;
        }
        var candidate = new ZLinkSerialWorkItem(
            callback,
            relocationRelease,
            previousOwnerMessageFollow,
            ZLinkSerialWorkLane.Application,
            acceptedSequence,
            payload,
            payloadFactory,
            byteCost: byteCost);

        if (!CommitWorkItemUnderLock(
            destination,
            candidate,
            ZLinkSerialWorkLane.Application,
            bypassCapacity: true))
        {
            item = null!;
            scheduleDrain = false;
            return false;
        }
        _nextAcceptedSequence = acceptedSequence + 1;

        item = candidate;
        scheduleDrain = _relocation is null;
        return true;
    }

    private bool CanReserveUnderLock(
        ZLinkSerialWorkLane lane,
        long byteCost)
    {
        var messageCapacity = lane == ZLinkSerialWorkLane.Application
            ? _policy.ApplicationMessageCapacity
            : _policy.LifecycleMessageCapacity;
        var byteCapacity = lane == ZLinkSerialWorkLane.Application
            ? _policy.ApplicationByteCapacity
            : _policy.LifecycleByteCapacity;
        ref var pendingCount = ref PendingCount(lane);
        ref var pendingBytes = ref PendingBytes(lane);
        return byteCost >= 0
               && pendingCount < messageCapacity
               && byteCost <= byteCapacity - pendingBytes;
    }

    private bool TryGetByteCost(
        long payloadBytes,
        long metadataBytes,
        out long byteCost)
    {
        try
        {
            byteCost = checked(
                checked(payloadBytes + metadataBytes)
                + _policy.FixedWorkByteCost);
            return payloadBytes >= 0 && metadataBytes >= 0;
        }
        catch (OverflowException)
        {
            byteCost = 0;
            return false;
        }
    }

    private void CommitReservationUnderLock(
        ZLinkSerialWorkLane lane,
        long byteCost,
        TaskCompletionSource? applicationDrained = null)
    {
        ref var pendingCount = ref PendingCount(lane);
        ref var pendingBytes = ref PendingBytes(lane);
        if (applicationDrained is not null)
            _applicationDrained = applicationDrained;
        pendingCount++;
        pendingBytes = checked(pendingBytes + byteCost);
        _pendingCount++;
    }

    private TaskCompletionSource? NewApplicationDrainedSignalUnderLock(
        ZLinkSerialWorkLane lane) =>
        lane == ZLinkSerialWorkLane.Application
        && _applicationPendingCount == 0
            ? new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously)
            : null;

    private void ReleaseReservedSlotUnderLock(
        ZLinkSerialWorkLane lane,
        long byteCost)
    {
        ref var pendingCount = ref PendingCount(lane);
        ref var pendingBytes = ref PendingBytes(lane);
        pendingCount--;
        pendingBytes = checked(pendingBytes - byteCost);
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

    private ref long PendingBytes(ZLinkSerialWorkLane lane)
    {
        if (lane == ZLinkSerialWorkLane.Application)
            return ref _applicationPendingBytes;
        return ref _lifecyclePendingBytes;
    }

    private static TaskCompletionSource CompletedSignal()
    {
        var signal = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        signal.TrySetResult();
        return signal;
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
            throw CreateAdmissionException("lifecycle", admission);
        await item.Completion.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private static ZLinkFrameworkException CreateAdmissionException(
        string lane,
        ZLinkSerialPostAdmission admission)
    {
        if (admission == ZLinkSerialPostAdmission.CapacityExceeded)
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                $"The serial {lane} queue has reached its configured capacity.");
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ShuttingDown,
            $"The serial {lane} queue is closed.");
    }

    private void ScheduleDrain()
    {
        if (Interlocked.Exchange(ref _drainScheduled, 1) != 0)
            return;

        if (!_taskRunner.TryRunDetached("serial-queue-drain", DrainAsync))
            _ = Task.Run(
                async () =>
                    await DrainAsync(CancellationToken.None)
                        .ConfigureAwait(false),
                CancellationToken.None);
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
                    _postResume,
                    _tryPostCallback,
                    _reportHandlerException,
                    _executionToken);
                await item.InvokeAsync(
                    _reportHandlerException,
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
                    CompleteSealRequestUnderLock();
                }
                if (Stopwatch.GetElapsedTime(sliceStartedAt)
                    >= _policy.OwnerTimeBudget)
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
        if (++_consecutiveLifecycleTurns >= _policy.LifecycleBurstLimit)
        {
            _consecutiveLifecycleTurns = 0;
            _lifecycleYieldDebt = true;
        }
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
        _nextAcceptedSequence = checked(
            _nextAcceptedSequence + (ulong)reservedAcceptedSequences);
        _relocation = new ZLinkRelocationQueueState(
            serial,
            captured,
            held,
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
            if (item.ReservationHeld)
                ReleaseReservedSlotUnderLock(item.Lane, item.ByteCost);
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
        var item = new ZLinkSerialWorkItem(async _ =>
        {
            turn.ResetSuspension();
            resume();
            var ownerTask = turn.OwnerTask;
            if (ownerTask is null || ownerTask.IsCompleted) return;

            await Task.WhenAny(ownerTask, turn.Suspended).ConfigureAwait(false);
        }, reservationHeld: false);
        lock (_admissionGate)
        {
            if (Volatile.Read(ref _completed) != 0)
                return ZLinkSerialPostAdmission.Closed;
            if (!CommitWorkItemUnderLock(
                _applicationQueue,
                item,
                ZLinkSerialWorkLane.Application,
                bypassCapacity: true))
                throw new InvalidOperationException(
                    "A suspended serial turn could not be resumed.");
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
        ZLinkSerialWorkQueue captured,
        ZLinkSerialWorkQueue held,
        ulong firstReservedSequence,
        int reservedAcceptedSequences)
    {
        public ulong Serial { get; } = serial;

        public ZLinkSerialWorkQueue Captured { get; } = captured;

        public ulong FirstReservedSequence { get; } =
            firstReservedSequence;

        public int ReservedAcceptedSequences { get; } =
            reservedAcceptedSequences;

        public ZLinkSerialWorkQueue Held { get; } = held;

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
    RelocationMoving = 2,
    CapacityExceeded = 3
}

internal enum ZLinkSerialPostAdmission
{
    Accepted = 0,
    Closed = 1,
    CapacityExceeded = 2
}
