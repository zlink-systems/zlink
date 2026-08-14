// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Owns fair, binding-level asynchronous admission for one PUB or XPUB
///     socket.
/// </summary>
/// <remarks>
///     Every fresh operation receives one immediate DONTWAIT attempt. A generic
///     send-ready callback snapshots the currently parked ring; each snapshotted
///     operation is attempted at most once for that readiness epoch and a
///     back-pressured operation rotates to the tail.
/// </remarks>
internal sealed class PublisherAdmissionScheduler
{
    private const int DontWaitFlag = 1;

    private readonly IntPtr _handle;
    private readonly object _submitGate;
    private readonly Func<int> _readSendTimeoutMs;
    private readonly object _gate = new();
    private readonly LinkedList<PendingOperation> _parked = new();
    private readonly HashSet<PendingOperation> _lifecycle = new();
    private readonly Queue<ReadyWork> _readyWork = new();
    private readonly SortedSet<DeadlineEntry> _deadlines = new(
        DeadlineEntryComparer.Instance);
    private readonly System.Threading.Timer _deadlineTimer;

    private bool _closing;
    private bool _pumpScheduled;
    private long _nextDeadlineSequence;
    private long _readyEpoch;

    internal PublisherAdmissionScheduler(IntPtr handle, object submitGate,
        Func<int> readSendTimeoutMs)
    {
        _handle = handle;
        _submitGate = submitGate
            ?? throw new ArgumentNullException(nameof(submitGate));
        _readSendTimeoutMs = readSendTimeoutMs
            ?? throw new ArgumentNullException(nameof(readSendTimeoutMs));
        _deadlineTimer = new System.Threading.Timer(static state =>
                ((PublisherAdmissionScheduler)state!).ProcessDeadlines(), this,
            System.Threading.Timeout.InfiniteTimeSpan,
            System.Threading.Timeout.InfiniteTimeSpan);
    }

    internal Task PublishAsync(string topic, IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        var started = Stopwatch.GetTimestamp();
        int sendTimeoutMs;
        lock (_gate)
        {
            ThrowIfClosingLocked();
            sendTimeoutMs = _readSendTimeoutMs();
        }

        var record = new PendingRecord(topic, parts);
        var operation = new PendingOperation(this, record,
            CreateDeadline(started, sendTimeoutMs), sendTimeoutMs == 0);
        operation.AttachCancellation(cancellationToken);
        operation.AttachDeadline();
        StartFresh(operation);
        return operation.Task;
    }

    internal void SignalReady()
    {
        lock (_gate)
        {
            if (_closing)
                return;

            var epoch = ++_readyEpoch;
            for (var node = _parked.First; node != null; node = node.Next)
                _readyWork.Enqueue(new ReadyWork(node.Value, epoch));

            foreach (var operation in _lifecycle)
                if (operation.NativeInFlight)
                    operation.DeferredEpochs.Enqueue(epoch);

            SchedulePumpLocked();
        }
    }

    internal void BeginClose()
    {
        List<(PendingOperation Operation, bool Release)>? terminal = null;
        lock (_gate)
        {
            if (_closing)
                return;

            _closing = true;
            _deadlineTimer.Change(System.Threading.Timeout.InfiniteTimeSpan,
                System.Threading.Timeout.InfiniteTimeSpan);
            foreach (var operation in _lifecycle.ToArray())
            {
                var release = !operation.NativeInFlight;
                if (operation.Node != null)
                {
                    _parked.Remove(operation.Node);
                    operation.Node = null;
                }

                operation.DeferredEpochs.Clear();
                (terminal ??= new List<(PendingOperation, bool)>()).Add(
                    (operation, release));
            }

            _readyWork.Clear();
        }

        _deadlineTimer.Dispose();
        if (terminal == null)
            return;
        foreach (var item in terminal)
        {
            item.Operation.CompleteClosed();
            if (item.Release)
                item.Operation.ReleaseRecord();
        }
    }

    internal void WaitForSubmitQuiescence()
    {
        lock (_submitGate)
        {
        }
    }

    private void StartFresh(PendingOperation operation)
    {
        var release = false;
        var close = false;
        lock (_gate)
        {
            if (operation.IsTerminal)
                release = true;
            else if (_closing)
            {
                close = true;
                release = true;
            }
            else
            {
                _lifecycle.Add(operation);
                operation.NativeInFlight = true;
            }
        }

        if (close)
            operation.CompleteClosed();
        if (release)
        {
            operation.ReleaseRecord();
            return;
        }

        Attempt(operation);
    }

    private void Attempt(PendingOperation operation)
    {
        PreparedRecordAttempt? attempt = null;
        int result;
        Exception? error = null;
        try
        {
            attempt = operation.PrepareAttempt();
            lock (_submitGate)
            {
                lock (_gate)
                {
                    if (_closing || operation.IsTerminal)
                    {
                        result = (int)SubmitResult.Terminated;
                        goto submitComplete;
                    }
                }

                result = operation.BeginAttempt()
                    ? attempt.Publish(_handle, operation.Record.TopicUtf8)
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
        CompleteAttempt(operation, result, error);
    }

    private void CompleteAttempt(PendingOperation operation, int result,
        Exception? error)
    {
        var accepted = result == (int)SubmitResult.Ok;
        var backpressured = result == (int)SubmitResult.Backpressured;
        var release = false;
        var timeout = false;
        var close = false;
        lock (_gate)
        {
            operation.NativeInFlight = false;
            if (accepted)
            {
                operation.NativeAccepted = true;
                operation.DeferredEpochs.Clear();
            }
            else if (backpressured && !operation.IsTerminal && !_closing
                     && !operation.AttemptOnceWithoutWaiting)
            {
                operation.Node = _parked.AddLast(operation);
                while (operation.DeferredEpochs.Count != 0)
                    _readyWork.Enqueue(new ReadyWork(operation,
                        operation.DeferredEpochs.Dequeue()));
                SchedulePumpLocked();
            }
            else
            {
                operation.DeferredEpochs.Clear();
                release = true;
                timeout = backpressured && !operation.IsTerminal && !_closing
                          && operation.AttemptOnceWithoutWaiting;
                close = _closing && !operation.IsTerminal;
            }
        }

        if (accepted)
        {
            operation.Accepted();
            return;
        }

        if (backpressured && !release)
            return;

        if (error != null)
            operation.CompleteException(error);
        else if (timeout)
            operation.CompleteTimedOut();
        else if (close)
            operation.CompleteClosed();
        else if (!operation.IsTerminal)
            operation.CompleteException(
                ZlinkException.CreateSubmitException((SubmitResult)result));

        if (release)
            operation.ReleaseRecord();
    }

    private void Pump()
    {
        while (true)
        {
            PendingOperation? operation = null;
            lock (_gate)
            {
                while (_readyWork.Count != 0)
                {
                    var work = _readyWork.Dequeue();
                    var candidate = work.Operation;
                    if (_closing || candidate.IsTerminal
                                 || candidate.Node == null
                                 || candidate.NativeInFlight)
                        continue;

                    _parked.Remove(candidate.Node);
                    candidate.Node = null;
                    candidate.NativeInFlight = true;
                    operation = candidate;
                    break;
                }

                if (operation == null)
                {
                    _pumpScheduled = false;
                    return;
                }
            }

            Attempt(operation);
        }
    }

    private void SchedulePumpLocked()
    {
        if (_pumpScheduled || _readyWork.Count == 0)
            return;
        _pumpScheduled = true;
        ThreadPool.UnsafeQueueUserWorkItem(static scheduler =>
            scheduler.Pump(), this, false);
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

    private void TerminateManaged(PendingOperation operation, Action complete)
    {
        var release = false;
        lock (_gate)
        {
            if (operation.Node != null && !operation.NativeInFlight)
            {
                _parked.Remove(operation.Node);
                operation.Node = null;
                release = true;
            }
            else if (!operation.NativeInFlight && !operation.NativeAccepted)
            {
                release = true;
            }

            operation.DeferredEpochs.Clear();
        }

        complete();
        if (release)
            operation.ReleaseRecord();
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

        var remainingTicks = _deadlines.Min!.Deadline
                             - Stopwatch.GetTimestamp();
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
        if (timeoutMs < 0)
            return long.MaxValue;
        var delta = Math.Ceiling(timeoutMs
                                 * (double)Stopwatch.Frequency / 1000d);
        return delta >= long.MaxValue - started
            ? long.MaxValue
            : started + (long)delta;
    }

    private void ThrowIfClosingLocked()
    {
        if (_closing)
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Terminated);
    }

    private readonly record struct ReadyWork(PendingOperation Operation,
        long Epoch);

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

    private sealed class PendingOperation
    {
        private readonly PublisherAdmissionScheduler _owner;
        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private CancellationTokenRegistration _cancellationRegistration;
        private int _recordReleased;
        private int _terminal;

        internal PendingOperation(PublisherAdmissionScheduler owner,
            PendingRecord record, long deadline,
            bool attemptOnceWithoutWaiting)
        {
            _owner = owner;
            Record = record;
            Deadline = deadline;
            AttemptOnceWithoutWaiting = attemptOnceWithoutWaiting;
        }

        internal Task Task => _completion.Task;
        internal PendingRecord Record { get; }
        internal bool IsTerminal => Volatile.Read(ref _terminal) != 0;
        internal bool NativeAccepted { get; set; }
        internal bool NativeInFlight { get; set; }
        internal bool Attempted { get; private set; }
        internal bool AttemptOnceWithoutWaiting { get; }
        internal long Deadline { get; }
        internal DeadlineEntry? DeadlineEntry { get; set; }
        internal LinkedListNode<PendingOperation>? Node { get; set; }
        internal Queue<long> DeferredEpochs { get; } = new();

        internal void AttachCancellation(CancellationToken token)
        {
            if (!token.CanBeCanceled)
                return;
            var registration = token.Register(static state =>
            {
                var pair = ((PendingOperation Operation,
                    CancellationToken Token))state!;
                pair.Operation._owner.Cancel(pair.Operation, pair.Token);
            }, (this, token));
            _cancellationRegistration = registration;
            if (IsTerminal)
                registration.Dispose();
        }

        internal void AttachDeadline()
        {
            _owner.RegisterDeadline(this);
        }

        internal bool BeginAttempt()
        {
            if (Deadline != long.MaxValue
                && Stopwatch.GetTimestamp() >= Deadline
                && !(AttemptOnceWithoutWaiting && !Attempted))
            {
                _owner.Timeout(this);
                return false;
            }

            Attempted = true;
            return true;
        }

        internal PreparedRecordAttempt PrepareAttempt()
        {
            return Record.PrepareAttempt();
        }

        internal void Accepted()
        {
            ReleaseRecord();
            if (TryBeginTerminal())
                _completion.TrySetResult();
        }

        internal void CompleteCanceled(CancellationToken token)
        {
            if (TryBeginTerminal())
                _completion.TrySetCanceled(token);
        }

        internal void CompleteTimedOut()
        {
            CompleteException(new ZlinkSubmitException(
                SubmitResult.Backpressured, (int)ErrorCode.ETimedOut));
        }

        internal void CompleteClosed()
        {
            CompleteException(new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Terminated));
        }

        internal void CompleteException(Exception error)
        {
            if (TryBeginTerminal())
                _completion.TrySetException(error);
        }

        internal void ReleaseRecord()
        {
            if (Interlocked.Exchange(ref _recordReleased, 1) == 0)
                Record.Dispose();
        }

        private bool TryBeginTerminal()
        {
            if (Interlocked.CompareExchange(ref _terminal, 1, 0) != 0)
                return false;
            _cancellationRegistration.Dispose();
            _owner.UnregisterTerminal(this);
            return true;
        }
    }

    private sealed class PendingRecord : IDisposable
    {
        private ZlinkMsg[]? _parts;

        internal PendingRecord(string topic, IReadOnlyList<Message> parts)
        {
            BoundaryValidation.ValidateTopicOrFilterUtf8(topic,
                nameof(topic));
            RequestReplySupport.EnsureParts(parts, nameof(parts));
            TopicUtf8 = PublishTopicEncoding.GetNullTerminatedUtf8(topic);

            Message[]? copied = null;
            var source = NativeMessageParts.AsSpan(parts, ref copied);
            var native = new ZlinkMsg[source.Length];
            var built = 0;
            try
            {
                NativeMessageParts.MoveToNative(source, native, nameof(parts),
                    ref built);
                _parts = native;
            }
            catch
            {
                NativeMessageParts.RestoreManaged(source, native, 0, built);
                throw;
            }
        }

        internal byte[] TopicUtf8 { get; }

        internal PreparedRecordAttempt PrepareAttempt()
        {
            return new PreparedRecordAttempt(_parts
                ?? throw new ObjectDisposedException(nameof(PendingRecord)));
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

        internal unsafe int Publish(IntPtr handle, byte[] topicUtf8)
        {
            var parts = _parts ?? throw new ObjectDisposedException(
                nameof(PreparedRecordAttempt));
            fixed (byte* topic = topicUtf8)
                for (var index = 0; index < parts.Length; index++)
                {
                    var partFlag = index + 1 < parts.Length
                        ? NativeMethods.ZlinkPartFlag.More
                        : NativeMethods.ZlinkPartFlag.Final;
                    var rc = NativeMethods.zlink_publish_part_utf8(handle,
                        topic, ref parts[index], DontWaitFlag, partFlag);
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
