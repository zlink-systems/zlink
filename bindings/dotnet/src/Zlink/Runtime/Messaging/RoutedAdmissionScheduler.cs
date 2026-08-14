// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;
using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Owns exact-target first admission for one DEALER, ROUTER, or STREAM socket.
/// </summary>
/// <remarks>
///     The Core callback only marks one target runnable. All native submits run
///     later on the shared thread pool. One short socket-owned gate protects a
///     complete exact-target DONTWAIT part loop; readiness waits never hold it.
/// </remarks>
internal sealed class RoutedAdmissionScheduler
{
    private const int DontWaitFlag = 1;

    private readonly IntPtr _handle;
    private readonly SocketType _socketType;
    private readonly Func<int> _readSendTimeoutMs;
    private readonly object _gate = new();
    private readonly object _selectionGate = new();
    private readonly object _completeRecordAttemptGate;
    private readonly SortedSet<DeadlineEntry> _deadlines = new(
        DeadlineEntryComparer.Instance);
    private readonly System.Threading.Timer _deadlineTimer;
    private readonly Dictionary<TargetKey, TargetState> _pending = new();
    private readonly HashSet<PendingOperation> _lifecycle = new();
    private readonly Queue<TargetKey> _readyTargets = new();
    private readonly NativeMethods.ZlinkRoutedSendReadyHandlerDelegate
        _readyHandler;

    private bool _closing;
    private bool _pumpScheduled;
    private long _nextDeadlineSequence;

    internal RoutedAdmissionScheduler(IntPtr handle, SocketType socketType,
        object completeRecordAttemptGate, Func<int> readSendTimeoutMs)
    {
        if (socketType is not (SocketType.Dealer or SocketType.Router
            or SocketType.Stream))
            throw new ArgumentOutOfRangeException(nameof(socketType));

        _handle = handle;
        _socketType = socketType;
        _completeRecordAttemptGate = completeRecordAttemptGate
            ?? throw new ArgumentNullException(nameof(completeRecordAttemptGate));
        _readSendTimeoutMs = readSendTimeoutMs
            ?? throw new ArgumentNullException(nameof(readSendTimeoutMs));
        _deadlineTimer = new System.Threading.Timer(static state =>
                ((RoutedAdmissionScheduler)state!).ProcessDeadlines(), this,
            System.Threading.Timeout.InfiniteTimeSpan,
            System.Threading.Timeout.InfiniteTimeSpan);
        _readyHandler = OnNativeReady;
        try
        {
            var rc = NativeMethods.zlink_routed_send_ready_handler(handle,
                _readyHandler, IntPtr.Zero);
            if (rc != 0)
                ZlinkException.ThrowHandlerIfError(rc);
        }
        catch
        {
            _deadlineTimer.Dispose();
            throw;
        }
    }

    internal Task SendAsync(RoutingId? routerRoutingId,
        IReadOnlyList<Message> parts, CancellationToken cancellationToken)
    {
        if (_socketType == SocketType.Stream && parts.Count != 1)
            throw new ArgumentException(
                "STREAM exact admission accepts one FINAL message; submit frames separately.",
                nameof(parts));
        var started = Stopwatch.GetTimestamp();
        int sendTimeoutMs;
        ZlinkRoutedSubmitTarget target;
        lock (_selectionGate)
        {
            ThrowIfClosing();
            sendTimeoutMs = _readSendTimeoutMs();
            target = SelectTarget(routerRoutingId);
        }
        var deadline = CreateDeadline(started, sendTimeoutMs);
        var record = new PendingRecord(parts, nameof(parts));
        var operation = new SendPendingOperation(this, target, record,
            deadline, sendTimeoutMs == 0);
        operation.AttachCancellation(cancellationToken);
        operation.AttachDeadline();
        Enqueue(operation);
        return operation.Task;
    }

    internal Task<IReadOnlyList<Message>> RequestAsync(
        RoutingId? routerRoutingId, IReadOnlyList<Message> parts,
        uint timeoutMs, CancellationToken cancellationToken)
    {
        var started = Stopwatch.GetTimestamp();
        var deadline = CreateDeadline(started, timeoutMs);
        ZlinkRoutedSubmitTarget target;
        lock (_selectionGate)
        {
            ThrowIfClosing();
            target = SelectTarget(routerRoutingId);
        }
        var record = new PendingRecord(parts, nameof(parts));
        var operation = new RequestPendingOperation(this, target, record,
            deadline);
        operation.AttachCancellation(cancellationToken);
        operation.AttachDeadline();
        Enqueue(operation);
        return RequestProgressPump.AttachSocket(_handle, operation.Task);
    }

    internal void BeginClose()
    {
        Dictionary<PendingOperation, bool>? terminal = null;
        lock (_gate)
        {
            if (_closing)
                return;

            _closing = true;
            _deadlineTimer.Change(System.Threading.Timeout.InfiniteTimeSpan,
                System.Threading.Timeout.InfiniteTimeSpan);
            foreach (var state in _pending.Values)
            {
                state.Terminal = true;
                var node = state.Operations.First;
                while (node != null)
                {
                    var next = node.Next;
                    var operation = node.Value;
                    var release = !ReferenceEquals(operation, state.InFlight);
                    if (release)
                    {
                        state.Operations.Remove(node);
                        operation.Node = null;
                    }

                    (terminal ??= new Dictionary<PendingOperation, bool>())
                        [operation] = release;
                    node = next;
                }
            }

            foreach (var operation in _lifecycle)
                (terminal ??= new Dictionary<PendingOperation, bool>())
                    .TryAdd(operation, false);

            foreach (var key in _pending
                         .Where(entry => entry.Value.InFlight == null)
                         .Select(entry => entry.Key)
                         .ToArray())
                _pending.Remove(key);
        }

        _deadlineTimer.Dispose();
        if (terminal == null)
            return;
        foreach (var item in terminal)
        {
            item.Key.CompleteClosed();
            if (item.Value)
                item.Key.ReleaseUnsubmitted();
        }
    }

    internal void WaitForSubmitQuiescence()
    {
        // BeginClose prevents new target selections and record attempts before
        // the native handle is closed. Neither gate is held while awaiting HWM.
        lock (_selectionGate)
        {
        }
        lock (_completeRecordAttemptGate)
        {
        }
    }

    private unsafe ZlinkRoutedSubmitTarget SelectTarget(
        RoutingId? routerRoutingId)
    {
        ZlinkRoutingId nativeRoutingId = default;
        IntPtr routingIdPointer = IntPtr.Zero;
        if (routerRoutingId.HasValue)
        {
            nativeRoutingId = routerRoutingId.Value.ToNative();
            routingIdPointer = (IntPtr)(&nativeRoutingId);
        }

        var rc = NativeMethods.zlink_select_routed_submit_target(_handle,
            routingIdPointer, out var target);
        if (rc != (int)SubmitResult.Ok)
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        return target;
    }

    private void ThrowIfClosing()
    {
        lock (_gate)
            if (_closing)
                throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Terminated);
    }

    private void Enqueue(PendingOperation operation)
    {
        var release = false;
        var close = false;
        lock (_gate)
        {
            if (operation.IsTerminal)
            {
                release = true;
            }
            else if (_closing)
            {
                close = true;
                release = true;
            }
            else
            {
                _lifecycle.Add(operation);
                var key = TargetKey.From(operation.Target);
                operation.Key = key;
                if (!_pending.TryGetValue(key, out var state))
                {
                    state = new TargetState();
                    _pending.Add(key, state);
                }

                operation.Node = state.Operations.AddLast(operation);
                if (state.Operations.Count == 1 && state.InFlight == null)
                    ScheduleTargetLocked(key, state);
            }
        }

        if (close)
            operation.CompleteClosed();
        if (release)
            operation.ReleaseUnsubmitted();
    }

    private void Cancel(PendingOperation operation,
        CancellationToken cancellationToken)
    {
        TerminateManaged(operation,
            () => operation.CompleteCanceled(cancellationToken));
    }

    private void Timeout(PendingOperation operation)
    {
        TerminateManaged(operation, operation.CompleteTimedOut);
    }

    private void RegisterDeadline(PendingOperation operation)
    {
        lock (_gate)
        {
            if (_closing || operation.IsTerminal
                         || operation.Deadline == long.MaxValue
                         || operation.AttemptOnceWithoutWaiting)
                return;
            var entry = new DeadlineEntry(operation.Deadline,
                ++_nextDeadlineSequence, operation);
            operation.DeadlineEntry = entry;
            _deadlines.Add(entry);
            ArmDeadlineTimerLocked();
        }
    }

    private void UnregisterTerminal(PendingOperation operation)
    {
        lock (_gate)
        {
            _lifecycle.Remove(operation);
            var entry = operation.DeadlineEntry;
            if (entry == null)
                return;
            operation.DeadlineEntry = null;
            _deadlines.Remove(entry);
            ArmDeadlineTimerLocked();
        }
    }

    private void ProcessDeadlines()
    {
        List<PendingOperation>? expired = null;
        lock (_gate)
        {
            if (_closing)
                return;

            var now = Stopwatch.GetTimestamp();
            while (_deadlines.Count != 0)
            {
                var entry = _deadlines.Min!;
                if (entry.Deadline > now)
                    break;

                _deadlines.Remove(entry);
                entry.Operation.DeadlineEntry = null;
                if (!entry.Operation.IsTerminal)
                    (expired ??= new List<PendingOperation>()).Add(
                        entry.Operation);
            }

            ArmDeadlineTimerLocked();
        }

        if (expired == null)
            return;
        foreach (var operation in expired)
            Timeout(operation);
    }

    private void ArmDeadlineTimerLocked()
    {
        if (_closing)
            return;
        if (_deadlines.Count == 0)
        {
            _deadlineTimer.Change(System.Threading.Timeout.InfiniteTimeSpan,
                System.Threading.Timeout.InfiniteTimeSpan);
            return;
        }

        var deadline = _deadlines.Min!.Deadline;
        var remainingTicks = deadline - Stopwatch.GetTimestamp();
        var dueMs = remainingTicks <= 0
            ? 0d
            : Math.Min(
                Math.Ceiling(remainingTicks * 1000d / Stopwatch.Frequency),
                uint.MaxValue - 1d);
        _deadlineTimer.Change(TimeSpan.FromMilliseconds(dueMs),
            System.Threading.Timeout.InfiniteTimeSpan);
    }

    private static long CreateDeadline(long started, int timeoutMs)
    {
        return timeoutMs < 0
            ? long.MaxValue
            : CreateDeadline(started, (uint)timeoutMs);
    }

    private static long CreateDeadline(long started, uint timeoutMs)
    {
        var delta = Math.Ceiling(timeoutMs
                                 * (double)Stopwatch.Frequency / 1000d);
        return delta >= long.MaxValue - started
            ? long.MaxValue
            : started + (long)delta;
    }

    private void TerminateManaged(PendingOperation operation,
        Action complete)
    {
        var release = false;
        lock (_gate)
        {
            if (operation.Node != null && !operation.NativeInFlight)
            {
                if (_pending.TryGetValue(operation.Key, out var state))
                {
                    var wasHead = ReferenceEquals(state.Operations.First,
                        operation.Node);
                    state.Operations.Remove(operation.Node);
                    operation.Node = null;
                    release = true;

                    if (state.Operations.Count == 0 && state.InFlight == null)
                        _pending.Remove(operation.Key);
                    else if (wasHead && state.InFlight == null
                                     && !state.Terminal)
                        ScheduleTargetLocked(operation.Key, state);
                }
                else
                {
                    operation.Node = null;
                    release = true;
                }
            }
            else if (operation.Node == null && !operation.NativeAccepted
                                            && !operation.NativeInFlight)
            {
                release = true;
            }
        }

        complete();
        if (release)
            operation.ReleaseUnsubmitted();
    }

    private unsafe void OnNativeReady(IntPtr subject, IntPtr eventPointer,
        IntPtr userData)
    {
        if (eventPointer == IntPtr.Zero)
            return;

        var readyEvent = *(ZlinkRoutedSendReadyEvent*)eventPointer;
        var routingId = RoutingId.FromNative(ref readyEvent.PeerRoutingId);
        if (!routingId.HasValue)
            return;

        var key = new TargetKey(routingId.Value,
            readyEvent.TransportPairId,
            readyEvent.TransportPairGeneration);
        if (readyEvent.State == ZlinkRoutedSendReadyState.Terminal)
        {
            SignalTerminal(key, readyEvent.TerminalErrno);
            return;
        }

        if (readyEvent.State != ZlinkRoutedSendReadyState.Writable)
            return;

        lock (_gate)
        {
            if (_closing || !_pending.TryGetValue(key, out var state)
                         || state.Terminal || state.Operations.Count == 0)
                return;
            if (state.InFlight != null)
            {
                state.WakePending = true;
                return;
            }

            ScheduleTargetLocked(key, state);
        }
    }

    private void SignalTerminal(TargetKey key, int errno)
    {
        List<(PendingOperation Operation, bool Release)>? terminal = null;
        lock (_gate)
        {
            if (!_pending.TryGetValue(key, out var state))
                return;

            state.Terminal = true;
            var node = state.Operations.First;
            while (node != null)
            {
                var next = node.Next;
                var operation = node.Value;
                var release = !ReferenceEquals(operation, state.InFlight);
                if (release)
                {
                    state.Operations.Remove(node);
                    operation.Node = null;
                }

                (terminal ??= new List<(PendingOperation, bool)>()).Add(
                    (operation, release));
                node = next;
            }

            if (state.InFlight == null)
                _pending.Remove(key);
        }

        if (terminal == null)
            return;
        foreach (var item in terminal)
        {
            item.Operation.CompleteRouteTerminal(errno);
            if (item.Release)
                item.Operation.ReleaseUnsubmitted();
        }
    }

    private void ScheduleTargetLocked(TargetKey key, TargetState state)
    {
        if (state.Scheduled || state.Terminal || state.Operations.Count == 0)
            return;
        state.Scheduled = true;
        _readyTargets.Enqueue(key);
        if (_pumpScheduled)
            return;

        _pumpScheduled = true;
        ThreadPool.UnsafeQueueUserWorkItem(static scheduler =>
                scheduler.Pump(), this, false);
    }

    private void Pump()
    {
        while (true)
        {
            PendingOperation? operation = null;
            TargetState? state = null;
            TargetKey key = default;
            lock (_gate)
            {
                while (_readyTargets.Count != 0)
                {
                    key = _readyTargets.Dequeue();
                    if (!_pending.TryGetValue(key, out state))
                        continue;
                    state.Scheduled = false;
                    if (_closing || state.Terminal
                                 || state.Operations.First == null
                                 || state.InFlight != null)
                        continue;

                    operation = state.Operations.First.Value;
                    state.InFlight = operation;
                    operation.NativeInFlight = true;
                    break;
                }

                if (operation == null)
                {
                    _pumpScheduled = false;
                    return;
                }
            }

            PreparedRecordAttempt? attempt = null;
            int result;
            Exception? error = null;
            try
            {
                attempt = operation.PrepareAttempt();
                lock (_completeRecordAttemptGate)
                {
                    lock (_gate)
                    {
                        if (_closing || state!.Terminal || operation.IsTerminal)
                        {
                            result = (int)SubmitResult.Terminated;
                            goto submitComplete;
                        }
                    }

                    result = operation.BeginAttempt()
                        ? operation.Submit(_handle, _socketType, attempt)
                        : (int)SubmitResult.Backpressured;
                }
            }
            catch (Exception exception)
            {
                result = (int)SubmitResult.InternalError;
                error = exception;
            }
            finally
            {
                attempt?.Dispose();
            }

        submitComplete:
            var accepted = result == (int)SubmitResult.Ok;
            var backpressured = result == (int)SubmitResult.Backpressured;
            var releaseUnsubmitted = false;
            lock (_gate)
            {
                operation.NativeInFlight = false;
                if (accepted)
                    operation.NativeAccepted = true;

                if (_pending.TryGetValue(key, out var current)
                    && ReferenceEquals(current, state)
                    && ReferenceEquals(current.InFlight, operation))
                {
                    current.InFlight = null;
                    current.WakePending = current.WakePending && backpressured;
                    if (!backpressured || operation.IsTerminal
                                       || current.Terminal || _closing)
                    {
                        if (operation.Node != null)
                        {
                            current.Operations.Remove(operation.Node);
                            operation.Node = null;
                        }

                        releaseUnsubmitted = !accepted;
                        current.WakePending = false;
                        if (current.Operations.Count == 0)
                            _pending.Remove(key);
                        else if (!current.Terminal && !_closing)
                            ScheduleTargetLocked(key, current);
                    }
                    else if (current.WakePending)
                    {
                        current.WakePending = false;
                        ScheduleTargetLocked(key, current);
                    }
                }
                else
                {
                    operation.Node = null;
                    releaseUnsubmitted = !accepted;
                }
            }

            if (accepted)
                operation.Accepted();
            else if (!backpressured || operation.IsTerminal || _closing
                                      || state!.Terminal)
            {
                if (error != null)
                    operation.CompleteException(error);
                else if (!operation.IsTerminal)
                    operation.CompleteSubmitFailure((SubmitResult)result);
                if (releaseUnsubmitted)
                    operation.ReleaseUnsubmitted();
            }
        }
    }

    private readonly record struct TargetKey(RoutingId RoutingId,
        ulong TransportPairId, ulong TransportPairGeneration)
    {
        internal static TargetKey From(ZlinkRoutedSubmitTarget target)
        {
            var routingId = RoutingId.FromNative(ref target.PeerRoutingId)
                            ?? throw new InvalidOperationException(
                                "Core selected an empty routed target.");
            return new TargetKey(routingId, target.TransportPairId,
                target.TransportPairGeneration);
        }
    }

    private sealed record DeadlineEntry(long Deadline, long Sequence,
        PendingOperation Operation);

    private sealed class DeadlineEntryComparer : IComparer<DeadlineEntry>
    {
        internal static DeadlineEntryComparer Instance { get; } = new();

        public int Compare(DeadlineEntry? left, DeadlineEntry? right)
        {
            if (ReferenceEquals(left, right))
                return 0;
            if (left == null)
                return -1;
            if (right == null)
                return 1;
            var deadline = left.Deadline.CompareTo(right.Deadline);
            return deadline != 0
                ? deadline
                : left.Sequence.CompareTo(right.Sequence);
        }
    }

    private sealed class TargetState
    {
        internal readonly LinkedList<PendingOperation> Operations = new();
        internal PendingOperation? InFlight;
        internal bool Scheduled;
        internal bool Terminal;
        internal bool WakePending;
    }

    private abstract class PendingOperation
    {
        private CancellationTokenRegistration _cancellationRegistration;
        private int _recordReleased;
        private int _terminal;

        protected PendingOperation(RoutedAdmissionScheduler owner,
            ZlinkRoutedSubmitTarget target, PendingRecord record,
            long deadline, bool attemptOnceWithoutWaiting = false)
        {
            Owner = owner;
            Target = target;
            Record = record;
            Deadline = deadline;
            AttemptOnceWithoutWaiting = attemptOnceWithoutWaiting;
        }

        protected RoutedAdmissionScheduler Owner { get; }
        protected PendingRecord Record { get; }
        internal bool IsTerminal => Volatile.Read(ref _terminal) != 0;
        internal TargetKey Key { get; set; }
        internal LinkedListNode<PendingOperation>? Node { get; set; }
        internal bool NativeAccepted { get; set; }
        internal bool NativeInFlight { get; set; }
        internal bool Attempted { get; private set; }
        internal bool AttemptOnceWithoutWaiting { get; }
        internal long Deadline { get; }
        internal DeadlineEntry? DeadlineEntry { get; set; }
        internal ZlinkRoutedSubmitTarget Target;

        internal void AttachCancellation(CancellationToken token)
        {
            if (!token.CanBeCanceled)
                return;
            var registration = token.Register(static state =>
            {
                var pair = ((PendingOperation Operation,
                    CancellationToken Token))state!;
                pair.Operation.Owner.Cancel(pair.Operation, pair.Token);
            }, (this, token));
            _cancellationRegistration = registration;
            if (IsTerminal)
                registration.Dispose();
        }

        internal PreparedRecordAttempt PrepareAttempt()
        {
            return Record.PrepareAttempt();
        }

        internal void AttachDeadline()
        {
            Owner.RegisterDeadline(this);
        }

        internal bool BeginAttempt()
        {
            if (Deadline != long.MaxValue
                && Stopwatch.GetTimestamp() >= Deadline
                && !(AttemptOnceWithoutWaiting && !Attempted))
            {
                Owner.Timeout(this);
                return false;
            }

            Attempted = true;
            return true;
        }

        internal abstract int Submit(IntPtr handle, SocketType socketType,
            PreparedRecordAttempt attempt);

        internal abstract void Accepted();
        internal abstract void CompleteTimedOut();

        internal void CompleteCanceled(CancellationToken token)
        {
            if (!TryBeginTerminal())
                return;
            CompleteCanceledCore(token);
        }

        internal virtual void CompleteClosed()
        {
            CompleteException(new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Terminated));
        }

        internal void CompleteRouteTerminal(int errno)
        {
            var result = errno switch
            {
                125 or 108 or 10058 or 156384765 =>
                    SubmitResult.Terminated,
                2 or 3 or 113 or 10065 => SubmitResult.NotFound,
                _ => SubmitResult.NotConnected
            };
            CompleteException(new ZlinkSubmitException(result, errno));
        }

        internal void CompleteSubmitFailure(SubmitResult result)
        {
            CompleteException(ZlinkException.CreateSubmitException(result));
        }

        internal void CompleteException(Exception error)
        {
            if (!TryBeginTerminal())
                return;
            CompleteExceptionCore(error);
        }

        internal void ReleaseUnsubmitted()
        {
            if (Interlocked.Exchange(ref _recordReleased, 1) != 0)
                return;
            Record.Dispose();
            ReleaseUnsubmittedCore();
        }

        protected void ReleaseAcceptedRecord()
        {
            if (Interlocked.Exchange(ref _recordReleased, 1) == 0)
                Record.Dispose();
        }

        protected bool TryBeginTerminal()
        {
            if (Interlocked.CompareExchange(ref _terminal, 1, 0) != 0)
                return false;
            _cancellationRegistration.Dispose();
            Owner.UnregisterTerminal(this);
            DisposeTerminalResources();
            return true;
        }

        protected virtual void DisposeTerminalResources()
        {
        }

        protected abstract void CompleteCanceledCore(CancellationToken token);
        protected abstract void CompleteExceptionCore(Exception error);

        protected virtual void ReleaseUnsubmittedCore()
        {
        }
    }

    private sealed class SendPendingOperation : PendingOperation
    {
        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        internal SendPendingOperation(RoutedAdmissionScheduler owner,
            ZlinkRoutedSubmitTarget target, PendingRecord record,
            long deadline, bool attemptOnceWithoutWaiting)
            : base(owner, target, record, deadline, attemptOnceWithoutWaiting)
        {
        }

        internal Task Task => _completion.Task;

        internal override int Submit(IntPtr handle, SocketType socketType,
            PreparedRecordAttempt attempt)
        {
            var result = attempt.Send(handle, socketType, ref Target);
            if (result == (int)SubmitResult.Backpressured
                && AttemptOnceWithoutWaiting)
                Owner.Timeout(this);
            return result;
        }

        internal override void Accepted()
        {
            ReleaseAcceptedRecord();
            if (TryBeginTerminal())
                _completion.TrySetResult();
        }

        internal override void CompleteTimedOut()
        {
            CompleteException(new ZlinkSubmitException(
                SubmitResult.Backpressured, (int)ErrorCode.ETimedOut));
        }

        protected override void CompleteCanceledCore(CancellationToken token)
        {
            _completion.TrySetCanceled(token);
        }

        protected override void CompleteExceptionCore(Exception error)
        {
            _completion.TrySetException(error);
        }
    }

    private sealed class RequestPendingOperation : PendingOperation
    {
        private static readonly NativeMethods.ZlinkReplyHandlerDelegate
            ReplyHandler = OnNativeReply;
        private static readonly IntPtr ReplyHandlerPointer =
            Marshal.GetFunctionPointerForDelegate(ReplyHandler);

        private readonly TaskCompletionSource<IReadOnlyList<Message>>
            _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly long _deadline;
        private GCHandle _selfHandle;
        private int _handleReleased;

        internal RequestPendingOperation(RoutedAdmissionScheduler owner,
            ZlinkRoutedSubmitTarget target, PendingRecord record,
            long deadline)
            : base(owner, target, record, deadline)
        {
            _deadline = deadline;
            _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
        }

        internal Task<IReadOnlyList<Message>> Task => _completion.Task;

        internal override int Submit(IntPtr handle, SocketType socketType,
            PreparedRecordAttempt attempt)
        {
            var remaining = RemainingTimeoutMs();
            if (remaining == 0)
            {
                Owner.Timeout(this);
                return (int)SubmitResult.Backpressured;
            }

            return attempt.Request(handle, socketType, ref Target, remaining,
                ReplyHandlerPointer, GCHandle.ToIntPtr(_selfHandle));
        }

        internal override void Accepted()
        {
            ReleaseAcceptedRecord();
        }

        internal override void CompleteClosed()
        {
            if (NativeAccepted)
            {
                CompleteException(new ZlinkRequestException(
                    ZlinkRequestException.ErrorCode.Terminated));
                return;
            }

            base.CompleteClosed();
        }

        internal override void CompleteTimedOut()
        {
            CompleteException(new ZlinkRequestException(
                ZlinkRequestException.ErrorCode.TimedOut));
        }

        protected override void CompleteCanceledCore(CancellationToken token)
        {
            _completion.TrySetCanceled(token);
        }

        protected override void CompleteExceptionCore(Exception error)
        {
            _completion.TrySetException(error);
        }

        protected override void ReleaseUnsubmittedCore()
        {
            FreeSelfHandle();
        }

        private uint RemainingTimeoutMs()
        {
            var remainingTicks = _deadline - Stopwatch.GetTimestamp();
            if (remainingTicks <= 0)
                return 0;
            var remainingMs = (uint)Math.Clamp(
                Math.Ceiling(remainingTicks * 1000d / Stopwatch.Frequency),
                1d, uint.MaxValue);
            return remainingMs;
        }

        private static void OnNativeReply(int result, IntPtr parts,
            nuint partCount, IntPtr userData)
        {
            var handle = GCHandle.FromIntPtr(userData);
            var operation = (RequestPendingOperation)handle.Target!;
            try
            {
                operation.CompleteNativeReply(result, ref parts,
                    ref partCount);
            }
            finally
            {
                if (parts != IntPtr.Zero)
                    NativeMethods.zlink_multipart_close(parts, partCount);
                operation.MarkHandleReleasedByNative();
                handle.Free();
            }
        }

        private void CompleteNativeReply(int result, ref IntPtr parts,
            ref nuint partCount)
        {
            if (result != 0)
            {
                CompleteException(new ZlinkRequestException(
                    (RequestResult)result));
                return;
            }

            if (IsTerminal)
                return;

            var messages = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            if (!TryBeginTerminal())
            {
                RequestReplySupport.DisposeParts(messages);
                return;
            }

            _completion.TrySetResult(messages);
        }

        private void FreeSelfHandle()
        {
            if (Interlocked.Exchange(ref _handleReleased, 1) != 0)
                return;
            if (_selfHandle.IsAllocated)
                _selfHandle.Free();
        }

        private void MarkHandleReleasedByNative()
        {
            Interlocked.Exchange(ref _handleReleased, 1);
        }
    }

    private sealed class PendingRecord : IDisposable
    {
        private ZlinkMsg[]? _parts;

        internal PendingRecord(IReadOnlyList<Message> parts, string paramName)
        {
            RequestReplySupport.EnsureParts(parts, paramName);
            Message[]? copied = null;
            var source = NativeMessageParts.AsSpan(parts, ref copied);
            var native = new ZlinkMsg[source.Length];
            var built = 0;
            try
            {
                NativeMessageParts.MoveToNative(source, native, paramName,
                    ref built);
                _parts = native;
            }
            catch
            {
                NativeMessageParts.RestoreManaged(source, native, 0, built);
                throw;
            }
        }

        internal PreparedRecordAttempt PrepareAttempt()
        {
            var parts = _parts
                        ?? throw new ObjectDisposedException(nameof(PendingRecord));
            return new PreparedRecordAttempt(parts);
        }

        public void Dispose()
        {
            var parts = Interlocked.Exchange(ref _parts, null);
            if (parts == null)
                return;
            for (var index = 0; index < parts.Length; index++)
                NativeMethods.zlink_msg_close(ref parts[index]);
        }
    }

    private sealed class PreparedRecordAttempt : IDisposable
    {
        private ZlinkMsg[]? _parts;

        internal PreparedRecordAttempt(ZlinkMsg[] source)
        {
            var copies = new ZlinkMsg[source.Length];
            var built = 0;
            try
            {
                for (var index = 0; index < source.Length; index++)
                {
                    var rc = NativeMethods.zlink_msg_init(ref copies[index]);
                    if (rc != 0)
                        throw ZlinkException.CreateConfigException(
                            NativeMethods.zlink_errno());
                    built++;
                    rc = NativeMethods.zlink_msg_copy(ref copies[index],
                        ref source[index]);
                    if (rc != 0)
                        throw ZlinkException.CreateConfigException(
                            NativeMethods.zlink_errno());
                }

                _parts = copies;
            }
            catch
            {
                for (var index = 0; index < built; index++)
                    NativeMethods.zlink_msg_close(ref copies[index]);
                throw;
            }
        }

        internal int Send(IntPtr handle, SocketType socketType,
            ref ZlinkRoutedSubmitTarget target)
        {
            var parts = _parts ?? throw new ObjectDisposedException(
                nameof(PreparedRecordAttempt));
            for (var index = 0; index < parts.Length; index++)
            {
                var partFlag = index + 1 < parts.Length
                    ? NativeMethods.ZlinkPartFlag.More
                    : NativeMethods.ZlinkPartFlag.Final;
                var rc = socketType == SocketType.Dealer
                    ? NativeMethods.zlink_dealer_send_transport_pair_part(
                        handle, ref target, ref parts[index], DontWaitFlag,
                        partFlag)
                    : NativeMethods.zlink_send_part_transport_pair(handle,
                        ref target.PeerRoutingId, target.TransportPairId,
                        target.TransportPairGeneration, ref parts[index],
                        DontWaitFlag, partFlag);
                if (rc != (int)SubmitResult.Ok)
                    return rc;
            }

            return (int)SubmitResult.Ok;
        }

        internal int Request(IntPtr handle, SocketType socketType,
            ref ZlinkRoutedSubmitTarget target, uint timeoutMs,
            IntPtr handler, IntPtr userData)
        {
            var parts = _parts ?? throw new ObjectDisposedException(
                nameof(PreparedRecordAttempt));
            for (var index = 0; index < parts.Length; index++)
            {
                var final = index + 1 == parts.Length;
                var partFlag = final
                    ? NativeMethods.ZlinkPartFlag.Final
                    : NativeMethods.ZlinkPartFlag.More;
                var partTimeout = final ? timeoutMs : 0;
                var partHandler = final ? handler : IntPtr.Zero;
                var partUserData = final ? userData : IntPtr.Zero;
                var rc = socketType == SocketType.Dealer
                    ? NativeMethods.zlink_dealer_request_transport_pair_part(
                        handle, ref target, ref parts[index], DontWaitFlag,
                        partFlag, partTimeout, partHandler, partUserData)
                    : NativeMethods.zlink_router_request_transport_pair_part(
                        handle, ref target.PeerRoutingId,
                        target.TransportPairId,
                        target.TransportPairGeneration, ref parts[index],
                        DontWaitFlag, partFlag, partTimeout, partHandler,
                        partUserData);
                if (rc != (int)SubmitResult.Ok)
                    return rc;
            }

            return (int)SubmitResult.Ok;
        }

        public void Dispose()
        {
            var parts = Interlocked.Exchange(ref _parts, null);
            if (parts == null)
                return;
            for (var index = 0; index < parts.Length; index++)
                NativeMethods.zlink_msg_close(ref parts[index]);
        }
    }
}
