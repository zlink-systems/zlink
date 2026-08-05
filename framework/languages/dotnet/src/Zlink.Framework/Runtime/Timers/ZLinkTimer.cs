namespace Zlink.Framework.Runtime.Timers;

internal sealed record ZLinkTimerLogicalSnapshot(
    string Name,
    TimeSpan Period,
    ZLinkTimerOptions Options,
    DateTimeOffset StartedAt,
    ulong DeliveryIndex,
    ulong LastScheduledIndex,
    DateTimeOffset? NextScheduledAt,
    ZLinkTimerTick? PendingTick);

internal sealed class ZLinkTimer : IZLinkTimer
{
    private readonly ZLinkTimerCallbacks _callbacks;
    private readonly object _lifecycleGate = new();
    private readonly object _scheduleGate = new();
    private readonly ZLinkTimerScheduler _scheduler;
    private readonly string _name;
    private readonly TimeSpan _period;
    private readonly CancellationTokenSource _stopSource;
    private DateTimeOffset _startedAt;
    private ulong _deliveryIndex;
    private ulong _lastScheduledIndex;
    private DateTimeOffset? _nextScheduledAt;
    private ZLinkTimerTick? _pendingTick;
    private TaskCompletionSource? _resume;
    private TaskCompletionSource? _activeDispatch;
    private Exception? _dispatchFailure;
    private Task? _finalization;
    private long _scheduleVersion;
    private int _disposed;

    public ZLinkTimer(
        string name,
        TimeSpan period,
        ZLinkTimerOptions options,
        CancellationToken spotStopToken,
        Func<ZLinkTimerTick, CancellationToken, ValueTask> onTickAsync,
        Func<ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask> onUnhandledExceptionAsync,
        Func<IDisposable>? enterTickScope = null,
        ZLinkTimerScheduler? scheduler = null)
        : this(
            new ZLinkTimerLogicalSnapshot(
                name,
                period,
                options,
                DateTimeOffset.UtcNow,
                0,
                0,
                null,
                null),
            spotStopToken,
            async (tick, cancellationToken) =>
            {
                await onTickAsync(tick, cancellationToken).ConfigureAwait(false);
                return true;
            },
            onUnhandledExceptionAsync,
            enterTickScope,
            false,
            scheduler)
    {
    }

    internal ZLinkTimer(
        ZLinkTimerLogicalSnapshot snapshot,
        CancellationToken spotStopToken,
        Func<ZLinkTimerTick, CancellationToken, ValueTask<bool>> onTickAsync,
        Func<ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask> onUnhandledExceptionAsync,
        Func<IDisposable>? enterTickScope = null,
        bool startFrozen = false,
        ZLinkTimerScheduler? scheduler = null)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        _scheduler = scheduler
            ?? throw new ArgumentNullException(nameof(scheduler));
        _name = snapshot.Name;
        _period = snapshot.Period;
        _callbacks = new ZLinkTimerCallbacks(
            snapshot.Options,
            onTickAsync,
            onUnhandledExceptionAsync,
            enterTickScope);
        _startedAt = snapshot.StartedAt;
        _deliveryIndex = snapshot.DeliveryIndex;
        _lastScheduledIndex = snapshot.LastScheduledIndex;
        _nextScheduledAt = snapshot.NextScheduledAt;
        _pendingTick = snapshot.PendingTick;
        if (startFrozen)
            _resume = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
        _stopSource = CancellationTokenSource.CreateLinkedTokenSource(spotStopToken);
        _scheduler.Register(this);

        SchedulerSchedule? schedule = null;
        var startDispatch = false;
        lock (_scheduleGate)
        {
            if (!startFrozen && _pendingTick is null)
            {
                schedule = PrepareScheduleUnderLock(
                    _nextScheduledAt ?? ComputeNextScheduledAtUnderLock());
            }
            else if (!startFrozen && _pendingTick is not null)
            {
                _activeDispatch = NewDispatchSource();
                startDispatch = true;
            }
        }
        PublishSchedule(schedule);
        if (startDispatch)
            StartPendingDispatch();
    }

    public bool IsDisposed => Volatile.Read(ref _disposed) != 0;

    internal ZLinkTimerLogicalSnapshot Freeze()
    {
        lock (_scheduleGate)
        {
            ObjectDisposedException.ThrowIf(IsDisposed, this);
            _resume ??= NewResumeSource();
            return SnapshotUnderLock();
        }
    }

    internal ZLinkTimerLogicalSnapshot Snapshot()
    {
        lock (_scheduleGate)
            return SnapshotUnderLock();
    }

    internal Task WaitForFrozenDispatchAsync()
    {
        lock (_scheduleGate)
        {
            if (_resume is null)
                throw new InvalidOperationException(
                    "A logical timer dispatch can be drained only while frozen.");
            return _activeDispatch?.Task ?? Task.CompletedTask;
        }
    }

    internal void RestoreFrozen(ZLinkTimerLogicalSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        lock (_scheduleGate)
        {
            ObjectDisposedException.ThrowIf(IsDisposed, this);
            if (_resume is null)
                throw new InvalidOperationException(
                    "A logical timer can only be restored while frozen.");
            if (!string.Equals(_name, snapshot.Name, StringComparison.Ordinal)
                || _period != snapshot.Period
                || _callbacks.Options != snapshot.Options)
                throw new InvalidDataException(
                    $"Logical timer '{snapshot.Name}' does not match its target registration.");

            _startedAt = snapshot.StartedAt;
            _deliveryIndex = snapshot.DeliveryIndex;
            _lastScheduledIndex = snapshot.LastScheduledIndex;
            _nextScheduledAt = snapshot.NextScheduledAt;
            _pendingTick = snapshot.PendingTick;
        }
    }

    internal bool IsScheduleCurrent(long version)
    {
        lock (_scheduleGate)
            return !IsDisposed && _scheduleVersion == version;
    }

    internal void NotifyDue(long version)
    {
        var startDispatch = false;
        lock (_scheduleGate)
        {
            if (IsDisposed
                || _scheduleVersion != version
                || _resume is not null)
                return;

            _nextScheduledAt = null;
            _pendingTick ??= CreateNextTickUnderLock(DateTimeOffset.UtcNow);
            if (_activeDispatch is null)
            {
                _activeDispatch = NewDispatchSource();
                startDispatch = true;
            }
        }

        if (startDispatch)
            StartPendingDispatch();
    }

    internal void Resume()
    {
        TaskCompletionSource? resume;
        SchedulerSchedule? schedule = null;
        var startDispatch = false;
        lock (_scheduleGate)
        {
            resume = _resume;
            _resume = null;
            if (resume is not null && !IsDisposed)
            {
                if (_pendingTick is not null)
                {
                    if (_activeDispatch is null)
                    {
                        _activeDispatch = NewDispatchSource();
                        startDispatch = true;
                    }
                }
                else
                {
                    schedule = PrepareScheduleUnderLock(
                        _nextScheduledAt ?? ComputeNextScheduledAtUnderLock());
                }
            }
        }

        resume?.TrySetResult();
        PublishSchedule(schedule);
        if (startDispatch)
            StartPendingDispatch();
    }

    public ValueTask CancelAsync() =>
        new(GetOrStartFinalization());

    public ValueTask DisposeAsync() => CancelAsync();

    private static TaskCompletionSource NewResumeSource() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static TaskCompletionSource NewDispatchSource() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private ZLinkTimerLogicalSnapshot SnapshotUnderLock()
    {
        return new ZLinkTimerLogicalSnapshot(
            _name,
            _period,
            _callbacks.Options,
            _startedAt,
            _deliveryIndex,
            _lastScheduledIndex,
            _nextScheduledAt,
            _pendingTick);
    }

    private Task GetOrStartFinalization()
    {
        TaskCompletionSource completion;
        lock (_lifecycleGate)
        {
            if (_finalization is not null)
                return _finalization;

            Volatile.Write(ref _disposed, 1);
            completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _finalization = completion.Task;
        }

        _ = CompleteFinalizationAsync(completion);
        return completion.Task;
    }

    private async Task CompleteFinalizationAsync(TaskCompletionSource completion)
    {
        try
        {
            await FinalizeAsync().ConfigureAwait(false);
            completion.TrySetResult();
        }
        catch (Exception exception)
        {
            completion.TrySetException(exception);
        }
    }

    private async Task FinalizeAsync()
    {
        List<Exception>? failures = null;
        try
        {
            _stopSource.Cancel();
            _scheduler.Unregister(this);
            Resume();
        }
        catch (Exception exception)
        {
            (failures ??= []).Add(exception);
        }

        try
        {
            Task? activeDispatch;
            lock (_scheduleGate)
                activeDispatch = _activeDispatch?.Task;
            if (activeDispatch is not null)
                await activeDispatch.ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            (failures ??= []).Add(exception);
        }

        try
        {
            _stopSource.Dispose();
        }
        catch (Exception exception)
        {
            (failures ??= []).Add(exception);
        }

        var dispatchFailure = Volatile.Read(ref _dispatchFailure);
        if (dispatchFailure is not null)
            (failures ??= []).Add(dispatchFailure);

        if (failures is [var failure])
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
        if (failures is { Count: > 1 })
            throw new AggregateException(failures);
    }

    private void StartPendingDispatch()
    {
        _ = Task.Run(DispatchPendingAsync);
    }

    private async Task DispatchPendingAsync()
    {
        TaskCompletionSource? dispatch;
        ZLinkTimerTick tick;
        lock (_scheduleGate)
        {
            dispatch = _activeDispatch;
            if (dispatch is null)
                return;
            if (IsDisposed
                || _resume is not null
                || _pendingTick is not { } pending)
            {
                _activeDispatch = null;
                dispatch.TrySetResult();
                return;
            }
            tick = pending;
        }

        ZLinkTimerDispatchOutcome outcome = default;
        Exception? failure = null;
        try
        {
            outcome = await _callbacks
                .DispatchTickAsync(tick, _stopSource.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (_stopSource.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            failure = exception;
        }

        SchedulerSchedule? nextSchedule = null;
        lock (_scheduleGate)
        {
            if (failure is not null)
            {
                _dispatchFailure ??= failure;
            }
            else if (_pendingTick == tick)
            {
                if (outcome.Delivered)
                {
                    if (outcome.KeepRunning)
                    {
                        _deliveryIndex = tick.DeliveryIndex;
                        _lastScheduledIndex = tick.ScheduledIndex;
                    }
                    _pendingTick = null;
                }

                if (!IsDisposed && _resume is null && outcome.KeepRunning)
                {
                    var due = outcome.Delivered
                        ? ComputeNextScheduledAtUnderLock()
                        : DateTimeOffset.UtcNow + _period;
                    nextSchedule = PrepareScheduleUnderLock(due);
                }
            }

            if (ReferenceEquals(_activeDispatch, dispatch))
                _activeDispatch = null;
        }
        dispatch.TrySetResult();
        PublishSchedule(nextSchedule);
    }

    private SchedulerSchedule PrepareScheduleUnderLock(DateTimeOffset dueAt)
    {
        var version = checked(++_scheduleVersion);
        _nextScheduledAt = dueAt;
        return new SchedulerSchedule(dueAt, version);
    }

    private void PublishSchedule(SchedulerSchedule? schedule)
    {
        if (schedule is { } value)
            _scheduler.Schedule(this, value.DueAt, value.Version);
    }

    private DateTimeOffset ComputeNextScheduledAtUnderLock()
    {
        if (_callbacks.Options.OverrunPolicy
            == ZLinkTimerOverrunPolicy.DelayNextTick)
            return DateTimeOffset.UtcNow + _period;

        return AddPeriods(_startedAt, _lastScheduledIndex + 1, _period);
    }

    private ZLinkTimerTick CreateNextTickUnderLock(DateTimeOffset startedAt)
    {
        ulong scheduledIndex;
        if (_callbacks.Options.OverrunPolicy
            == ZLinkTimerOverrunPolicy.DelayNextTick)
        {
            scheduledIndex = _lastScheduledIndex + 1;
        }
        else
        {
            var elapsedTicks = Math.Max(0, (startedAt - _startedAt).Ticks);
            var dueIndex = Math.Max(
                _lastScheduledIndex + 1,
                (ulong)Math.Max(1, elapsedTicks / _period.Ticks));
            scheduledIndex = SelectScheduledIndex(
                _callbacks.Options,
                _lastScheduledIndex,
                dueIndex);
        }

        var deliveryIndex = _deliveryIndex + 1;
        var scheduledAt = AddPeriods(_startedAt, scheduledIndex, _period);
        var scheduledElapsed = scheduledAt - _startedAt;
        var startedElapsed = startedAt - _startedAt;
        return new ZLinkTimerTick(
            _name,
            deliveryIndex,
            scheduledIndex,
            _period,
            scheduledAt,
            startedAt,
            scheduledElapsed,
            startedElapsed,
            startedElapsed - scheduledElapsed,
            scheduledIndex - _lastScheduledIndex - 1);
    }

    private static ulong SelectScheduledIndex(
        ZLinkTimerOptions options,
        ulong lastScheduledIndex,
        ulong dueScheduledIndex)
    {
        if (options.OverrunPolicy == ZLinkTimerOverrunPolicy.SkipLateTicks)
            return dueScheduledIndex;

        var availableTicks = dueScheduledIndex - lastScheduledIndex;
        var maxCatchUpTicks = (ulong)options.MaxCatchUpTicks;
        if (availableTicks > maxCatchUpTicks)
            return dueScheduledIndex - maxCatchUpTicks + 1;
        return lastScheduledIndex + 1;
    }

    private static DateTimeOffset AddPeriods(
        DateTimeOffset startedAt,
        ulong index,
        TimeSpan period)
    {
        var ticks = Math.Min(
            DateTimeOffset.MaxValue.Ticks - startedAt.Ticks,
            index * (double)period.Ticks);
        return startedAt.AddTicks((long)ticks);
    }

    private readonly struct ZLinkTimerCallbacks(
        ZLinkTimerOptions options,
        Func<ZLinkTimerTick, CancellationToken, ValueTask<bool>> onTickAsync,
        Func<ZLinkTimerTick, Exception, bool, CancellationToken, ValueTask> onUnhandledExceptionAsync,
        Func<IDisposable>? enterTickScope)
    {
        public ZLinkTimerOptions Options => options;

        public async ValueTask<ZLinkTimerDispatchOutcome> DispatchTickAsync(
            ZLinkTimerTick tick,
            CancellationToken cancellationToken)
        {
            using var tickScope = enterTickScope?.Invoke();
            try
            {
                var delivered = await onTickAsync(tick, cancellationToken)
                    .ConfigureAwait(false);
                return new ZLinkTimerDispatchOutcome(true, delivered);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception ex)
            {
                var stopped = options.StopOnUnhandledException;
                try
                {
                    await onUnhandledExceptionAsync(tick, ex, stopped, cancellationToken)
                        .ConfigureAwait(false);
                }
                catch
                {
                    // Monitoring failures must not change the timer exception policy.
                }

                return new ZLinkTimerDispatchOutcome(!stopped, true);
            }
        }
    }

    private readonly record struct SchedulerSchedule(
        DateTimeOffset DueAt,
        long Version);

    private readonly record struct ZLinkTimerDispatchOutcome(
        bool KeepRunning,
        bool Delivered);
}
