using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Messaging;

internal interface IZLinkDeadlineEntry<T>
    where T : class
{
    long AbsoluteDeadline { get; }
    T? DeadlineNext { get; set; }
    T? DeadlinePrevious { get; set; }
}

// The sole implementation of spec 01 §10.1. The owner supplies clock units,
// serialization, terminal decisions and the visit execution mechanism.
internal sealed class ZLinkDeadlineIndex<T>(long rotation)
    where T : class, IZLinkDeadlineEntry<T>
{
    internal T? Head { get; private set; }
    private T? _tail;
    private T? _cursor;
    private long _lastVisit;
    private long _lowerBound;
    internal long NextVisit { get; private set; } = long.MaxValue;

    internal void Register(T entry, long now)
    {
        if (NextVisit == long.MaxValue)
            _lastVisit = now;
        entry.DeadlinePrevious = _tail;
        if (_tail is null)
            Head = entry;
        else
            _tail.DeadlineNext = entry;
        _tail = entry;
        if (_cursor is not null)
            _lowerBound = Math.Min(_lowerBound, entry.AbsoluteDeadline);
        NextVisit = Math.Min(NextVisit, Math.Min(_lastVisit + rotation, entry.AbsoluteDeadline));
    }

    internal void Remove(T entry)
    {
        if (entry.DeadlinePrevious is null && !ReferenceEquals(Head, entry))
            return;
        if (entry.DeadlinePrevious is { } previous)
            previous.DeadlineNext = entry.DeadlineNext;
        else
            Head = entry.DeadlineNext;
        if (entry.DeadlineNext is { } next)
            next.DeadlinePrevious = entry.DeadlinePrevious;
        else
            _tail = entry.DeadlinePrevious;
        if (ReferenceEquals(_cursor, entry))
            _cursor = entry.DeadlineNext;
        entry.DeadlineNext = entry.DeadlinePrevious = null;
        // A terminal never cancels an already reserved visit.
    }

    internal void RollbackRegistration(T entry, long previousVisit)
    {
        Remove(entry);
        NextVisit = previousVisit;
    }

    internal void Visit(
        long now,
        Func<T, long, bool> visit,
        Func<bool> budgetSpent,
        Func<long> clock
    )
    {
        if (_cursor is null)
        {
            _lastVisit = now;
            _lowerBound = now + rotation;
            _cursor = Head;
        }
        while (_cursor is { } entry)
        {
            _cursor = entry.DeadlineNext;
            if (!visit(entry, clock()))
                Remove(entry);
            else
                _lowerBound = Math.Min(_lowerBound, entry.AbsoluteDeadline);
            if (budgetSpent())
                break;
        }
        // Only the visit decides to stop. A budgeted remainder is already due.
        NextVisit =
            Head is null ? long.MaxValue
            : _cursor is not null ? _lastVisit
            : _lowerBound;
    }
}

// ClientServer owner adapter: async lane registration, one reusable management
// timer, and one atomic terminal table. MeshNode uses the same index without a timer.
internal sealed class ZLinkRequestDeadlineIndex : IAsyncDisposable
{
    internal static readonly TimeSpan DefaultRotation = TimeSpan.FromMilliseconds(256);

    private readonly ZLinkStateLane _lane;
    private readonly TimeProvider _time;
    private readonly IZLinkRuntimeFailureReporter _errors;
    private readonly ConcurrentDictionary<Entry, byte> _pending = new();
    private readonly ZLinkDeadlineIndex<Entry> _index;
    private ITimer? _timer;
    private long _visitStarted;

    internal ZLinkRequestDeadlineIndex(
        ZLinkStateLane lane,
        TimeProvider? time = null,
        IZLinkRuntimeFailureReporter? errors = null
    )
    {
        _lane = lane;
        _time = time ?? TimeProvider.System;
        _errors = errors ?? new ZLinkRuntimeErrorSink();
        _index = new((long)Math.Ceiling(DefaultRotation.TotalSeconds * _time.TimestampFrequency));
        _timer = _time.CreateTimer(
            static state => ((ZLinkRequestDeadlineIndex)state!).OnTimer(),
            this,
            Timeout.InfiniteTimeSpan,
            Timeout.InfiniteTimeSpan
        );
    }

    internal async ValueTask<(Entry Entry, Task<IReadOnlyList<Message>> Completion)> RegisterAsync(
        TimeSpan timeout,
        Entry? entry = null
    )
    {
        entry ??= new Entry(this, timeout);
        var completion = entry.Source!.Task;
        await _lane
            .RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(_timer is null, this);
                var previous = _index.NextVisit;
                var now = _time.GetTimestamp();
                _pending.TryAdd(entry, 0);
                _index.Register(entry, now);
                try
                {
                    if (_index.NextVisit < previous)
                        Schedule();
                }
                catch
                {
                    _pending.TryRemove(entry, out _);
                    _index.RollbackRegistration(entry, previous);
                    throw;
                }
            })
            .ConfigureAwait(false);
        return (entry, completion);
    }

    internal Task<IReadOnlyList<Message>> RunAsync(
        TimeSpan timeout,
        CancellationToken cancellation,
        Func<Entry, Task<IReadOnlyList<Message>>, Task<IReadOnlyList<Message>>> produce,
        IReadOnlyList<Message>? sourceParts = null
    )
    {
        Entry entry;
        try
        {
            entry = new Entry(this, timeout);
        }
        catch
        {
            if (sourceParts is not null)
                ZLinkMessageParts.DisposeAll(sourceParts);
            throw;
        }
        var completion = entry.Source!.Task;
        _ = ProduceAsync(entry, completion, timeout, cancellation, produce, sourceParts);
        return completion;
    }

    private async Task ProduceAsync(
        Entry entry,
        Task<IReadOnlyList<Message>> completion,
        TimeSpan timeout,
        CancellationToken cancellation,
        Func<Entry, Task<IReadOnlyList<Message>>, Task<IReadOnlyList<Message>>> produce,
        IReadOnlyList<Message>? sourceParts
    )
    {
        try
        {
            await RegisterAsync(timeout, entry).ConfigureAwait(false);
            try
            {
                using var registration = cancellation.Register(() =>
                    entry.Fail(new OperationCanceledException(cancellation))
                );
                if (entry.IsPending)
                    entry.Complete(await produce(entry, completion).ConfigureAwait(false));
            }
            catch (Exception error)
            {
                entry.Fail(error);
            }
            return;
        }
        catch (Exception error)
        {
            entry.Error = error;
        }
        finally
        {
            if (sourceParts is not null)
                ZLinkMessageParts.DisposeAll(sourceParts);
        }
        // Registration failed before the entry acquired terminal authority.
        ZLinkCompletionDispatcher.Post(entry);
    }

    internal int PendingCount => _pending.Count;

    private void Schedule()
    {
        if (_index.NextVisit == long.MaxValue)
            return;
        var remaining = _time.GetElapsedTime(_time.GetTimestamp(), _index.NextVisit);
        _timer?.Change(
            remaining <= TimeSpan.Zero ? TimeSpan.Zero : remaining,
            Timeout.InfiniteTimeSpan
        );
    }

    private void OnTimer() =>
        ZLinkUnawaitedSubmit.Observe(_lane.RunAsync(Visit), "request-deadline-index", _errors);

    private bool VisitEntry(Entry entry, long now)
    {
        if (!entry.IsPending)
            return false;
        if (entry.Deadline > now)
            return true;
        entry.Fail(new ZlinkRequestException(ZlinkRequestException.ErrorCode.TimedOut));
        return false;
    }

    private bool BudgetSpent() =>
        _time.GetElapsedTime(_visitStarted).TotalMilliseconds
        >= Zlink.Framework.Runtime.Dispatch.ZLinkReceiveBatchBudget.MaximumMilliseconds;

    private long Now() => _time.GetTimestamp();

    private void Visit()
    {
        if (_timer is null)
            return;
        _visitStarted = Now();
        _index.Visit(_visitStarted, VisitEntry, BudgetSpent, Now);
        Schedule();
    }

    public async ValueTask DisposeAsync()
    {
        if (_timer is { } timer)
            await timer.DisposeAsync().ConfigureAwait(false);
        await _lane
            .RunAsync(() =>
            {
                _timer = null;
                while (_index.Head is { } entry)
                {
                    entry.Fail(
                        ZLinkRequestFailureMapper.CreateShutdownRequestException(
                            "Request owner stopped."
                        )
                    );
                    _index.Remove(entry);
                }
            })
            .ConfigureAwait(false);
    }

    internal sealed class Entry : ZLinkCompletionDispatcher.WorkItem, IZLinkDeadlineEntry<Entry>
    {
        private readonly ZLinkRequestDeadlineIndex _owner;
        internal readonly long Deadline;
        public Entry? DeadlineNext { get; set; }
        public Entry? DeadlinePrevious { get; set; }
        long IZLinkDeadlineEntry<Entry>.AbsoluteDeadline => Deadline;
        internal TaskCompletionSource<IReadOnlyList<Message>>? Source = new();
        private IReadOnlyList<Message>? _parts;
        internal Exception? Error;

        internal Entry(ZLinkRequestDeadlineIndex owner, TimeSpan timeout)
        {
            _owner = owner;
            Deadline =
                owner._time.GetTimestamp()
                + (long)Math.Ceiling(timeout.TotalSeconds * owner._time.TimestampFrequency);
        }

        internal bool IsPending => _owner._pending.ContainsKey(this);
        internal TimeSpan Remaining =>
            _owner._time.GetElapsedTime(_owner._time.GetTimestamp(), Deadline);

        internal void Complete(IReadOnlyList<Message> parts)
        {
            if (!_owner._pending.TryRemove(this, out _))
            {
                ZLinkMessageParts.DisposeAll(parts);
                return;
            }
            _parts = parts;
            ZLinkCompletionDispatcher.Post(this);
        }

        internal void Fail(Exception error)
        {
            if (!_owner._pending.TryRemove(this, out _))
                return;
            Error = error;
            ZLinkCompletionDispatcher.Post(this);
        }

        internal override void Execute()
        {
            if (Error is OperationCanceledException canceled)
                Source!.TrySetCanceled(canceled.CancellationToken);
            else if (Error is { } error)
                Source!.TrySetException(error);
            else
                Source!.TrySetResult(_parts!);
        }

        internal override void Completed(Exception? failure)
        {
            Source = null;
            _parts = null;
            Error = null;
            if (failure is not null)
                ZLinkFrameworkDebugLog.TaskFailure("request-dispatch", failure);
        }
    }
}
