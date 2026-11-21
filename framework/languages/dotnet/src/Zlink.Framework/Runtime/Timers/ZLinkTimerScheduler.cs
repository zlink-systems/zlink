using Zlink.Framework.Runtime.Execution;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Timers;

// A runtime generation owns one scheduler for all logical Spot timers.  The
// scheduler owns the single deadline wait; a timer only owns state for its
// current callback while that callback is being dispatched.
internal sealed class ZLinkTimerScheduler : IAsyncDisposable
{
    private readonly ZLinkStateLane _lane = new();
    private readonly PriorityQueue<ScheduledTimer, ZLinkTimerScheduleKey> _queue = new();
    private readonly HashSet<ZLinkTimer> _timers = [];
    private readonly SemaphoreSlim _wake = new(0, 1);
    private readonly CancellationTokenSource _stopSource = new();
    private readonly Task _pump;
    private Task? _disposeTask;
    private long _nextSequence;
    private bool _closed;
    internal TimeProvider TimeProvider { get; }
    internal TimeSpan Elapsed => TimeProvider.GetElapsedTime(0, TimeProvider.GetTimestamp());

    public ZLinkTimerScheduler(TimeProvider? timeProvider = null)
    {
        TimeProvider = timeProvider ?? TimeProvider.System;
        _pump = RunAsync(_stopSource.Token);
    }

    internal int TimerCount => AwaitStateLane(_lane.RunAsync(() => _timers.Count));

    internal int ScheduledEntryCount => AwaitStateLane(_lane.RunAsync(() => _queue.Count));

    internal void Register(ZLinkTimer timer)
    {
        ArgumentNullException.ThrowIfNull(timer);
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                ObjectDisposedException.ThrowIf(_closed, this);
                _timers.Add(timer);
            })
        );
    }

    internal ValueTask UnregisterAsync(ZLinkTimer timer) =>
        _lane.RunAsync(() =>
        {
            _timers.Remove(timer);
            if (!_closed)
                SignalWakeOnLane();
        });

    internal void Schedule(ZLinkTimer timer, TimeSpan dueAt, long version)
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_closed)
                    return;
                _queue.Enqueue(
                    new ScheduledTimer(timer, version),
                    new ZLinkTimerScheduleKey(dueAt.Ticks, ++_nextSequence)
                );
                SignalWakeOnLane();
            })
        );
    }

    public async ValueTask DisposeAsync()
    {
        var disposeTask = await _lane.RunAsync(GetOrStartDispose).ConfigureAwait(false);
        await disposeTask.ConfigureAwait(false);
    }

    private Task GetOrStartDispose()
    {
        if (_disposeTask is not null)
            return _disposeTask;

        _closed = true;
        _disposeTask = ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(() =>
            Task.Run(DisposeCoreAsync)
        );
        return _disposeTask;
    }

    private async Task DisposeCoreAsync()
    {
        try
        {
            _stopSource.Cancel();
            await _pump.ConfigureAwait(false);
        }
        finally
        {
            _wake.Dispose();
            _stopSource.Dispose();
            await _lane
                .RunAsync(() =>
                {
                    _queue.Clear();
                    _timers.Clear();
                })
                .ConfigureAwait(false);
        }
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        Task? wake = null;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var next = await _lane.RunAsync(TryTakeDueOnLane).ConfigureAwait(false);
                if (next.HasDue)
                {
                    await next
                        .DueTimer.Timer.NotifyDueAsync(next.DueTimer.Version)
                        .ConfigureAwait(false);
                    continue;
                }

                wake ??= _wake.WaitAsync(cancellationToken);
                if (next.Delay is null)
                {
                    await wake.ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
                    wake = null;
                    continue;
                }

                using var waitCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken
                );
                var deadline = Task.Delay(next.Delay.Value, TimeProvider, waitCancellation.Token);
                await Task.WhenAny(wake, deadline).ConfigureAwait(false);
                if (wake.IsCompleted)
                {
                    await wake.ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
                    wake = null;
                }
                waitCancellation.Cancel();
                await deadline.ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
        finally
        {
            _stopSource.Cancel();
            if (wake is not null)
                await wake.ConfigureAwait(ConfigureAwaitOptions.SuppressThrowing);
        }
    }

    private (bool HasDue, ScheduledTimer DueTimer, TimeSpan? Delay) TryTakeDueOnLane()
    {
        if (_queue.Count == 0)
        {
            return (false, default, null);
        }

        _queue.TryPeek(out _, out var key);
        var nowTicks = Elapsed.Ticks;
        if (key.ElapsedTicks > nowTicks)
            return (false, default, TimeSpan.FromTicks(key.ElapsedTicks - nowTicks));

        return (true, _queue.Dequeue(), TimeSpan.Zero);
    }

    private void SignalWakeOnLane()
    {
        if (_wake.CurrentCount == 0)
            _wake.Release();
    }

    private readonly record struct ScheduledTimer(ZLinkTimer Timer, long Version);

    private readonly record struct ZLinkTimerScheduleKey(long ElapsedTicks, long Sequence)
        : IComparable<ZLinkTimerScheduleKey>
    {
        public int CompareTo(ZLinkTimerScheduleKey other)
        {
            var byDeadline = ElapsedTicks.CompareTo(other.ElapsedTicks);
            return byDeadline != 0 ? byDeadline : Sequence.CompareTo(other.Sequence);
        }
    }
}
