using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.UnitTests;

public sealed class TimerLifecycleTests
{
    [Fact]
    public async Task Timer_freeze_preserves_logical_cursor_and_resume_dispatches_due_tick()
    {
        await using var scheduler = new ZLinkTimerScheduler();
        var tick = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var timer = new ZLinkTimer(
            "relocatable",
            TimeSpan.FromMilliseconds(30),
            new ZLinkTimerOptions(),
            CancellationToken.None,
            (value, _) =>
            {
                tick.TrySetResult(value);
                return ValueTask.CompletedTask;
            },
            static (_, _, _, _) => ValueTask.CompletedTask,
            scheduler: scheduler);

        var frozen = timer.Freeze();
        Assert.Equal("relocatable", frozen.Name);
        Assert.Equal<ulong>(0, frozen.DeliveryIndex);
        Assert.Null(frozen.PendingTick);
        await Task.Delay(80);
        Assert.False(tick.Task.IsCompleted);

        timer.Resume();
        var delivered = await tick.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal<ulong>(1, delivered.DeliveryIndex);
        Assert.True(delivered.ScheduledIndex >= 1);
        Assert.Equal(delivered.ScheduledIndex - 1, delivered.SkippedTicks);
        await timer.DisposeAsync();
    }

    [Fact]
    public async Task Timer_freeze_commits_tick_that_started_before_freeze()
    {
        await using var scheduler = new ZLinkTimerScheduler();
        var tickStarted = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseTick = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var timer = new ZLinkTimer(
            "pending",
            TimeSpan.FromMilliseconds(1),
            new ZLinkTimerOptions(),
            CancellationToken.None,
            async (value, _) =>
            {
                tickStarted.TrySetResult(value);
                await releaseTick.Task.ConfigureAwait(false);
            },
            static (_, _, _, _) => ValueTask.CompletedTask,
            scheduler: scheduler);

        var started = await tickStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var frozen = timer.Freeze();
        Assert.Equal(started, frozen.PendingTick);
        Assert.Equal<ulong>(0, frozen.DeliveryIndex);

        releaseTick.TrySetResult();
        await WaitUntilAsync(() => timer.Snapshot().PendingTick is null);
        var completed = timer.Snapshot();
        Assert.Null(completed.PendingTick);
        Assert.Equal(started.DeliveryIndex, completed.DeliveryIndex);
        Assert.Equal(started.ScheduledIndex, completed.LastScheduledIndex);
        await timer.DisposeAsync();
    }

    [Fact]
    public async Task Frozen_relocation_snapshot_waits_for_active_tick_before_reading_cursor()
    {
        var tickStarted = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseTick = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var registry = new ZLinkSpotTimerRegistry(static () => false);
        _ = await registry.AddAsync(
            "quiescent",
            TimeSpan.FromMilliseconds(1),
            null,
            typeof(TestTimerHandler),
            typeof(TestTimerSpot),
            CancellationToken.None,
            async (_, tick, _) =>
            {
                tickStarted.TrySetResult(tick);
                await releaseTick.Task.ConfigureAwait(false);
                return true;
            },
            static (_, _, _, _, _) => ValueTask.CompletedTask,
            CancellationToken.None);

        var activeTick = await tickStarted.Task.WaitAsync(
            TimeSpan.FromSeconds(5));
        var boundary = registry.FreezeRelocation();
        Assert.Equal(
            activeTick,
            ZLinkSpotTimerRelocationCodec.Decode(
                Assert.Single(boundary)).Timer.PendingTick);
        var quiescent = registry
            .SnapshotFrozenRelocationAfterDispatchesAsync(
                CancellationToken.None)
            .AsTask();
        Assert.False(quiescent.IsCompleted);

        releaseTick.TrySetResult();
        var snapshot = await quiescent.WaitAsync(TimeSpan.FromSeconds(5));
        var timer = ZLinkSpotTimerRelocationCodec.Decode(
            Assert.Single(snapshot)).Timer;
        Assert.Null(timer.PendingTick);
        Assert.Equal(activeTick.DeliveryIndex, timer.DeliveryIndex);
        Assert.Equal(activeTick.ScheduledIndex, timer.LastScheduledIndex);
        await registry.DisposeAsync();
    }

    [Fact]
    public async Task Timer_freeze_keeps_tick_pending_when_dispatch_is_suppressed_after_freeze()
    {
        await using var scheduler = new ZLinkTimerScheduler();
        var tickStarted = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseDispatch = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var dispatchReturned = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var timer = new ZLinkTimer(
            new ZLinkTimerLogicalSnapshot(
                "suppressed",
                TimeSpan.FromMilliseconds(1),
                new ZLinkTimerOptions(),
                DateTimeOffset.UtcNow,
                0,
                0,
                null,
                null),
            CancellationToken.None,
            async (value, _) =>
            {
                tickStarted.TrySetResult(value);
                await releaseDispatch.Task.ConfigureAwait(false);
                dispatchReturned.TrySetResult();
                return false;
            },
            static (_, _, _, _) => ValueTask.CompletedTask,
            scheduler: scheduler);

        var started = await tickStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var frozen = timer.Freeze();
        Assert.Equal(started, frozen.PendingTick);

        releaseDispatch.TrySetResult();
        await dispatchReturned.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.Delay(20);
        var suppressed = timer.Snapshot();
        Assert.Equal(started, suppressed.PendingTick);
        Assert.Equal<ulong>(0, suppressed.DeliveryIndex);
        await timer.DisposeAsync();
    }

    [Fact]
    public async Task Concurrent_cancel_and_dispose_wait_for_the_same_blocked_timer_pump()
    {
        await using var scheduler = new ZLinkTimerScheduler();
        var tickStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseTick = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var timer = new ZLinkTimer(
            "blocked",
            TimeSpan.FromMilliseconds(1),
            new ZLinkTimerOptions(),
            CancellationToken.None,
            async (_, _) =>
            {
                tickStarted.TrySetResult();
                await releaseTick.Task.ConfigureAwait(false);
            },
            static (_, _, _, _) => ValueTask.CompletedTask,
            scheduler: scheduler);

        await tickStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var cancel = timer.CancelAsync().AsTask();
        var dispose = timer.DisposeAsync().AsTask();
        Assert.False(cancel.IsCompleted);
        Assert.False(dispose.IsCompleted);
        Assert.True(timer.IsDisposed);

        releaseTick.TrySetResult();
        await Task.WhenAll(cancel, dispose).WaitAsync(TimeSpan.FromSeconds(5));

        await timer.CancelAsync();
        await timer.DisposeAsync();
    }

    [Fact]
    public async Task Concurrent_cancel_callers_observe_the_same_cleanup_failure_after_pump_completion()
    {
        await using var scheduler = new ZLinkTimerScheduler();
        var tickStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseTick = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var timer = new ZLinkTimer(
            "failing-cancel",
            TimeSpan.FromMilliseconds(1),
            new ZLinkTimerOptions(),
            CancellationToken.None,
            async (_, cancellationToken) =>
            {
                using var registration = cancellationToken.Register(
                    static () => throw new InvalidOperationException("cancel callback failed"));
                tickStarted.TrySetResult();
                await releaseTick.Task.ConfigureAwait(false);
            },
            static (_, _, _, _) => ValueTask.CompletedTask,
            scheduler: scheduler);
        await tickStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var first = timer.CancelAsync().AsTask();
        var second = timer.CancelAsync().AsTask();
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);

        releaseTick.TrySetResult();
        var firstFailure = await Assert.ThrowsAsync<AggregateException>(() => first);
        var secondFailure = await Assert.ThrowsAsync<AggregateException>(() => second);
        Assert.Contains(firstFailure.InnerExceptions, static error => error is InvalidOperationException);
        Assert.Same(firstFailure, secondFailure);
    }

    [Fact]
    public async Task Registry_close_rejects_new_timers_and_finalizes_every_admitted_timer()
    {
        var registry = new ZLinkSpotTimerRegistry(static () => false);
        var admitted = new List<IZLinkTimer>();
        for (var index = 0; index < 32; index++)
        {
            admitted.Add(await AddTimerAsync(registry, $"timer-{index}"));
        }

        var first = registry.DisposeAsync().AsTask();
        var second = registry.DisposeAsync().AsTask();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.All(admitted, static timer => Assert.True(timer.IsDisposed));
        await Assert.ThrowsAsync<ObjectDisposedException>(
            async () => await AddTimerAsync(registry, "after-close"));
    }

    [Fact]
    public async Task One_scheduler_owns_deadlines_for_all_registered_timers()
    {
        await using var scheduler = new ZLinkTimerScheduler();
        var registry = new ZLinkSpotTimerRegistry(
            static () => false,
            scheduler: scheduler);
        for (var index = 0; index < 32; index++)
            await AddTimerAsync(registry, $"shared-{index}");

        Assert.Equal(32, scheduler.TimerCount);
        Assert.Equal(32, scheduler.ScheduledEntryCount);

        await registry.DisposeAsync();
        Assert.Equal(0, scheduler.TimerCount);
    }

    [Fact]
    public async Task Registry_relocation_roundtrip_restores_registration_and_cursor_frozen()
    {
        var source = new ZLinkSpotTimerRegistry(static () => false);
        await AddTimerAsync(source, "logical");

        var relocation = source.FreezeRelocation();
        Assert.Single(relocation);
        var sourceSnapshot = ZLinkSpotTimerRelocationCodec.Decode(relocation[0]);
        Assert.Equal(typeof(TestTimerHandler), sourceSnapshot.HandlerType);
        Assert.Equal(typeof(TestTimerSpot), sourceSnapshot.SpotType);

        var target = new ZLinkSpotTimerRegistry(static () => false);
        target.RestoreRelocation(
            relocation,
            CancellationToken.None,
            static (_, _, _) => ValueTask.FromResult(true),
            static (_, _, _, _, _) => ValueTask.CompletedTask);
        var restored = target.FreezeRelocation();
        var targetSnapshot = ZLinkSpotTimerRelocationCodec.Decode(
            Assert.Single(restored));

        Assert.Equal(sourceSnapshot.Timer.Name, targetSnapshot.Timer.Name);
        Assert.Equal(sourceSnapshot.Timer.Period, targetSnapshot.Timer.Period);
        Assert.Equal(
            sourceSnapshot.Timer.DeliveryIndex,
            targetSnapshot.Timer.DeliveryIndex);
        Assert.Equal(
            sourceSnapshot.Timer.LastScheduledIndex,
            targetSnapshot.Timer.LastScheduledIndex);

        source.Resume();
        target.Resume();
        await source.DisposeAsync();
        await target.DisposeAsync();
    }

    [Fact]
    public async Task Admission_seal_freeze_preserves_one_pending_tick_without_retry_spin()
    {
        var registry = new ZLinkSpotTimerRegistry(static () => false);
        var firstTick = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var resumedTick = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var attempts = 0;
        var failureReports = 0;
        var admissionOpen = 0;
        _ = await registry.AddAsync(
            "admission-seal",
            TimeSpan.FromMilliseconds(1),
            null,
            typeof(TestTimerHandler),
            typeof(TestTimerSpot),
            CancellationToken.None,
            (_, tick, _) =>
            {
                var attempt = Interlocked.Increment(ref attempts);
                if (Volatile.Read(ref admissionOpen) == 0)
                {
                    registry.FreezeForApplicationAdmissionSeal();
                    firstTick.TrySetResult(tick);
                    return ValueTask.FromResult(false);
                }
                registry.FreezeForApplicationAdmissionSeal();
                resumedTick.TrySetResult(tick);
                return ValueTask.FromResult(true);
            },
            (_, _, _, _, _) =>
            {
                Interlocked.Increment(ref failureReports);
                return ValueTask.CompletedTask;
            },
            CancellationToken.None);

        var pending = await firstTick.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.Delay(50);
        var frozen = ZLinkSpotTimerRelocationCodec.Decode(
            Assert.Single(registry.FreezeRelocation()));

        Assert.Equal(1, Volatile.Read(ref attempts));
        Assert.Equal(0, Volatile.Read(ref failureReports));
        Assert.Equal(pending, frozen.Timer.PendingTick);
        Assert.Equal<ulong>(0, frozen.Timer.DeliveryIndex);

        Volatile.Write(ref admissionOpen, 1);
        registry.Resume();
        var delivered = await resumedTick.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(pending, delivered);
        Assert.Equal(2, Volatile.Read(ref attempts));
        await registry.DisposeAsync();
    }

    [Fact]
    public async Task Admission_sealed_pending_tick_is_restored_once_on_the_target()
    {
        var source = new ZLinkSpotTimerRegistry(static () => false);
        var sourceTick = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _ = await source.AddAsync(
            "admission-seal-target",
            TimeSpan.FromMilliseconds(1),
            null,
            typeof(TestTimerHandler),
            typeof(TestTimerSpot),
            CancellationToken.None,
            (_, tick, _) =>
            {
                source.FreezeForApplicationAdmissionSeal();
                sourceTick.TrySetResult(tick);
                return ValueTask.FromResult(false);
            },
            static (_, _, _, _, _) => ValueTask.CompletedTask,
            CancellationToken.None);

        var pending = await sourceTick.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var relocation = source.FreezeRelocation();
        var captured = ZLinkSpotTimerRelocationCodec.Decode(
            Assert.Single(relocation));
        Assert.Equal(pending, captured.Timer.PendingTick);

        var targetTick = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var targetDeliveries = 0;
        var targetFailures = 0;
        var target = new ZLinkSpotTimerRegistry(static () => false);
        target.RestoreRelocation(
            relocation,
            CancellationToken.None,
            (_, tick, _) =>
            {
                Interlocked.Increment(ref targetDeliveries);
                target.FreezeForApplicationAdmissionSeal();
                targetTick.TrySetResult(tick);
                return ValueTask.FromResult(true);
            },
            (_, _, _, _, _) =>
            {
                Interlocked.Increment(ref targetFailures);
                return ValueTask.CompletedTask;
            });

        target.Resume();
        var restored = await targetTick.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(pending, restored);
        await WaitUntilAsync(
            () => ZLinkSpotTimerRelocationCodec
                .Decode(Assert.Single(target.FreezeRelocation()))
                .Timer.PendingTick is null);
        Assert.Equal(1, Volatile.Read(ref targetDeliveries));
        Assert.Equal(0, Volatile.Read(ref targetFailures));

        await source.DisposeAsync();
        await target.DisposeAsync();
    }

    [Fact]
    public async Task Relocation_target_restores_configured_timer_in_place_and_keeps_it_frozen()
    {
        var source = new ZLinkSpotTimerRegistry(static () => false);
        await AddTimerAsync(source, "configured");
        await AddTimerAsync(source, "dynamic-source-only");
        var relocation = source.FreezeRelocation();

        var deliveries = 0;
        var target = new ZLinkSpotTimerRegistry(
            static () => false,
            restorePending: true);
        var configured = await target.AddAsync(
            "configured",
            TimeSpan.FromHours(1),
            null,
            typeof(TestTimerHandler),
            typeof(TestTimerSpot),
            CancellationToken.None,
            (_, _, _) =>
            {
                Interlocked.Increment(ref deliveries);
                return ValueTask.FromResult(true);
            },
            static (_, _, _, _, _) => ValueTask.CompletedTask,
            CancellationToken.None);

        target.RestoreRelocation(
            relocation,
            typeof(TestTimerSpot),
            CancellationToken.None,
            (_, _, _) =>
            {
                Interlocked.Increment(ref deliveries);
                return ValueTask.FromResult(true);
            },
            static (_, _, _, _, _) => ValueTask.CompletedTask);

        Assert.False(configured.IsDisposed);
        Assert.Equal(0, Volatile.Read(ref deliveries));
        var restored = target.FreezeRelocation()
            .Select(static timer =>
                ZLinkSpotTimerRelocationCodec.Decode(timer))
            .ToDictionary(static snapshot => snapshot.Timer.Name);
        var expected = relocation
            .Select(static timer =>
                ZLinkSpotTimerRelocationCodec.Decode(timer))
            .ToDictionary(static snapshot => snapshot.Timer.Name);
        Assert.Equal(expected.Keys.Order(), restored.Keys.Order());
        Assert.Equal(
            expected["configured"].Timer,
            restored["configured"].Timer);
        Assert.Equal(
            expected["dynamic-source-only"].Timer,
            restored["dynamic-source-only"].Timer);

        source.Resume();
        target.Resume();
        await source.DisposeAsync();
        await target.DisposeAsync();
    }

    [Fact]
    public async Task Add_timer_racing_registry_dispose_is_either_rejected_or_fully_finalized()
    {
        for (var iteration = 0; iteration < 100; iteration++)
        {
            var registry = new ZLinkSpotTimerRegistry(static () => false);
            var start = new ManualResetEventSlim();
            var add = Task.Run(async () =>
            {
                start.Wait();
                try
                {
                    return await AddTimerAsync(registry, $"race-{iteration}");
                }
                catch (ObjectDisposedException)
                {
                    return null;
                }
            });
            var dispose = Task.Run(async () =>
            {
                start.Wait();
                await registry.DisposeAsync();
            });

            start.Set();
            await Task.WhenAll(add, dispose).WaitAsync(TimeSpan.FromSeconds(5));
            if (await add is { } timer) Assert.True(timer.IsDisposed);
            await registry.DisposeAsync();
        }
    }

    private static ValueTask<IZLinkTimer> AddTimerAsync(
        ZLinkSpotTimerRegistry registry,
        string name)
    {
        return registry.AddAsync(
            name,
            TimeSpan.FromHours(1),
            null,
            typeof(TestTimerHandler),
            typeof(TestTimerSpot),
            CancellationToken.None,
            static (_, _, _) => ValueTask.FromResult(true),
            static (_, _, _, _, _) => ValueTask.CompletedTask,
            CancellationToken.None);
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var timeoutAt = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
        while (!condition())
        {
            if (DateTimeOffset.UtcNow >= timeoutAt)
                throw new TimeoutException("The expected timer state was not reached.");
            await Task.Delay(10);
        }
    }

    private sealed class TestTimerSpot;

    private sealed class TestTimerHandler : IZLinkSpotTimerHandler<TestTimerSpot>
    {
        public ValueTask HandleAsync(
            TestTimerSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }
}
