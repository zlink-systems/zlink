using System.Collections.Concurrent;
using System.Diagnostics;

namespace Zlink.Framework.UnitTests;

public sealed class SerialExecutorLayerContractTests
{
    private static readonly TimeSpan TestTimeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task SubmissionPaths_SelectQueuesWithoutAnApplicationQueueArgument()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var spot = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        await using var session = new ZLinkSessionSerialExecutor(
            new object(),
            errorSink);
        var observed = new ConcurrentQueue<string>();

        await spot.ExecuteAsync(
            (_, _) =>
            {
                observed.Enqueue("spot");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None);
        await spot.ExecuteActorAsync(
            "actor-a",
            static (_, state, _) =>
            {
                state.Enqueue("actor");
                return ValueTask.CompletedTask;
            },
            observed,
            CancellationToken.None);
        await spot.ExecuteTimerAsync(
            "tick",
            static (_, state, _) =>
            {
                state.Enqueue("timer");
                return ValueTask.CompletedTask;
            },
            observed,
            CancellationToken.None);
        await spot.ExecuteLifecycleAsync(
            (_, _) =>
            {
                observed.Enqueue("lifecycle");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None);

        var sessionRan = NewSignal();
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            session.ExecuteApplication(
                _ =>
                {
                    observed.Enqueue("session");
                    sessionRan.TrySetResult();
                    return ValueTask.CompletedTask;
                }));
        await sessionRan.Task.WaitAsync(TestTimeout);

        Assert.Equal(
            new[] { "spot", "actor", "timer", "lifecycle", "session" },
            observed.ToArray());
    }

    [Fact]
    public async Task PerActor_TwoActorHandlersOverlap()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        var release = NewSignal();
        var bothStarted = NewSignal();
        var startedCount = 0;

        async ValueTask BlockAsync()
        {
            if (Interlocked.Increment(ref startedCount) == 2)
                bothStarted.TrySetResult();
            await release.Task.ConfigureAwait(false);
        }

        var first = ExecuteActorAsync(executor, "actor-a", BlockAsync);
        var second = ExecuteActorAsync(executor, "actor-b", BlockAsync);

        await bothStarted.Task.WaitAsync(TestTimeout);
        Assert.Equal(2, Volatile.Read(ref startedCount));

        release.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TestTimeout);
    }

    [Fact]
    public async Task SpotWide_StartsNextActorOnlyAfterThePreviousActorFinishes()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var firstStarted = NewSignal();
        var releaseFirst = NewSignal();
        var secondStarted = NewSignal();
        var order = new ConcurrentQueue<string>();

        var first = ExecuteActorAsync(
            executor,
            "actor-a",
            async () =>
            {
                order.Enqueue("first:start");
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
                order.Enqueue("first:end");
            });
        await firstStarted.Task.WaitAsync(TestTimeout);

        var second = ExecuteActorAsync(
            executor,
            "actor-b",
            () =>
            {
                order.Enqueue("second:start");
                secondStarted.TrySetResult();
                return ValueTask.CompletedTask;
            });

        Assert.NotSame(
            secondStarted.Task,
            await Task.WhenAny(secondStarted.Task, Task.Delay(100)));

        releaseFirst.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TestTimeout);
        Assert.Equal(
            new[] { "first:start", "first:end", "second:start" },
            order.ToArray());
    }

    [Fact]
    public async Task SameActor_RetainsSubmissionOrderInBothExecutionModes()
    {
        foreach (var mode in new[]
                 {
                     ZLinkUserSpotExecutionMode.PerActor,
                     ZLinkUserSpotExecutionMode.SpotWide
                 })
        {
            using var errorSink = new ZLinkRuntimeErrorSink();
            await using var executor = CreateSpotExecutor(errorSink, mode);
            var firstStarted = NewSignal();
            var releaseFirst = NewSignal();
            var secondStarted = NewSignal();
            var order = new ConcurrentQueue<string>();

            var first = ExecuteActorAsync(
                executor,
                "actor-a",
                async () =>
                {
                    order.Enqueue("first:start");
                    firstStarted.TrySetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                    order.Enqueue("first:end");
                });
            await firstStarted.Task.WaitAsync(TestTimeout);
            var second = ExecuteActorAsync(
                executor,
                "actor-a",
                () =>
                {
                    order.Enqueue("second");
                    secondStarted.TrySetResult();
                    return ValueTask.CompletedTask;
                });

            Assert.NotSame(
                secondStarted.Task,
                await Task.WhenAny(secondStarted.Task, Task.Delay(100)));
            releaseFirst.TrySetResult();
            await Task.WhenAll(first, second).WaitAsync(TestTimeout);
            Assert.Equal(
                new[] { "first:start", "first:end", "second" },
                order.ToArray());
        }
    }

    [Fact]
    public async Task PerActor_TimerNamesOverlapWhileOneNameRetainsFifo()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        var tickStarted = NewSignal();
        var releaseTick = NewSignal();
        var tickSecondStarted = NewSignal();
        var beatStarted = NewSignal();
        var order = new ConcurrentQueue<string>();

        var tickFirst = ExecuteTimerAsync(
            executor,
            "tick",
            async () =>
            {
                order.Enqueue("tick:first:start");
                tickStarted.TrySetResult();
                await releaseTick.Task.ConfigureAwait(false);
                order.Enqueue("tick:first:end");
            });
        await tickStarted.Task.WaitAsync(TestTimeout);
        var tickSecond = ExecuteTimerAsync(
            executor,
            "tick",
            () =>
            {
                order.Enqueue("tick:second");
                tickSecondStarted.TrySetResult();
                return ValueTask.CompletedTask;
            });
        var beat = ExecuteTimerAsync(
            executor,
            "beat",
            () =>
            {
                order.Enqueue("beat");
                beatStarted.TrySetResult();
                return ValueTask.CompletedTask;
            });

        await beatStarted.Task.WaitAsync(TestTimeout);
        Assert.False(tickSecondStarted.Task.IsCompleted);
        releaseTick.TrySetResult();
        await Task.WhenAll(tickFirst, tickSecond, beat).WaitAsync(TestTimeout);
        Assert.Equal(
            new[] { "tick:first:start", "beat", "tick:first:end", "tick:second" },
            order.ToArray());
    }

    [Fact]
    public async Task ActorMailboxCapacity_RejectsOnlyTheFullActorInBothModes()
    {
        foreach (var mode in new[]
                 {
                     ZLinkUserSpotExecutionMode.PerActor,
                     ZLinkUserSpotExecutionMode.SpotWide
                 })
        {
            using var errorSink = new ZLinkRuntimeErrorSink();
            await using var executor = CreateSpotExecutor(
                errorSink,
                mode,
                actorLanePolicy: CreatePolicy(
                    applicationMessageCapacity: 1,
                    applicationByteCapacity: 1_024));
            var firstStarted = NewSignal();
            var releaseFirst = NewSignal();

            var first = ExecuteActorAsync(
                executor,
                "actor-a",
                async () =>
                {
                    firstStarted.TrySetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                });
            await firstStarted.Task.WaitAsync(TestTimeout);

            await AssertCapacityExceededAsync(
                () => ExecuteActorAsync(
                    executor,
                    "actor-a",
                    static () => ValueTask.CompletedTask));

            var otherActor = ExecuteActorAsync(
                executor,
                "actor-b",
                static () => ValueTask.CompletedTask);
            releaseFirst.TrySetResult();
            await Task.WhenAll(first, otherActor).WaitAsync(TestTimeout);
        }
    }

    [Fact]
    public async Task SpotWide_ActorMailboxRejectsLargePayloadBeforeSmallPayload()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide,
            actorLanePolicy: CreatePolicy(
                applicationMessageCapacity: 8,
                applicationByteCapacity: 100));
        var firstStarted = NewSignal();
        var releaseFirst = NewSignal();

        var first = ExecuteActorWithPayloadAsync(
            executor,
            "actor-a",
            async () =>
            {
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            payloadBytes: 60);
        await firstStarted.Task.WaitAsync(TestTimeout);

        await AssertCapacityExceededAsync(
            () => ExecuteActorWithPayloadAsync(
                executor,
                "actor-a",
                static () => ValueTask.CompletedTask,
                payloadBytes: 60));
        var small = ExecuteActorWithPayloadAsync(
            executor,
            "actor-a",
            static () => ValueTask.CompletedTask,
            payloadBytes: 10);

        releaseFirst.TrySetResult();
        await Task.WhenAll(first, small).WaitAsync(TestTimeout);
    }

    [Fact]
    public async Task SpotWide_UpperQueueSaturatesByCountForSmallAndLargePayloads()
    {
        await AssertUpperQueueRejectsSecondActorAsync(payloadBytes: 1);
        await AssertUpperQueueRejectsSecondActorAsync(payloadBytes: 10_000);
    }

    [Fact]
    public async Task LifecycleWorkOvertakesApplicationOnlyToLifecycleBurstLimit()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide,
            spotLanePolicy: CreatePolicy(
                applicationMessageCapacity: 8,
                applicationByteCapacity: 1_024,
                lifecycleMessageCapacity: 8,
                lifecycleByteCapacity: 1_024,
                lifecycleBurstLimit: 2));
        var blockerStarted = NewSignal();
        var releaseBlocker = NewSignal();
        var order = new ConcurrentQueue<string>();

        var blocker = executor.ExecuteAsync(
            async (_, _) =>
            {
                blockerStarted.TrySetResult();
                await releaseBlocker.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();
        await blockerStarted.Task.WaitAsync(TestTimeout);

        var application = executor.ExecuteAsync(
            (_, _) =>
            {
                order.Enqueue("application");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();
        var lifecycle = Enumerable.Range(1, 3)
            .Select(index => executor.ExecuteLifecycleAsync(
                    (_, _) =>
                    {
                        order.Enqueue($"lifecycle-{index}");
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None)
                .AsTask())
            .ToArray();

        releaseBlocker.TrySetResult();
        await Task.WhenAll(lifecycle.Append(application).Append(blocker))
            .WaitAsync(TestTimeout);
        Assert.Equal(
            new[] { "lifecycle-1", "lifecycle-2", "application", "lifecycle-3" },
            order.ToArray());
    }

    [Fact]
    public async Task OwnerTimeBudget_YieldsAnOverloadedActorBeforeItsRemainingRecordsRun()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor,
            actorLanePolicy: CreatePolicy(
                applicationMessageCapacity: 8,
                applicationByteCapacity: 1_024,
                ownerTimeBudget: TimeSpan.FromMilliseconds(1)));
        using var releaseFirst = new ManualResetEventSlim();
        var firstStarted = NewSignal();
        var events = new ConcurrentQueue<string>();

        var first = ExecuteActorAsync(
            executor,
            "actor-a",
            () =>
            {
                firstStarted.TrySetResult();
                Assert.True(releaseFirst.Wait(TestTimeout));
                BusyWait(TimeSpan.FromMilliseconds(2));
                events.Enqueue("actor-a:0");
                return ValueTask.CompletedTask;
            });
        await firstStarted.Task.WaitAsync(TestTimeout);

        var remaining = Enumerable.Range(1, 7)
            .Select(index => ExecuteActorAsync(
                executor,
                "actor-a",
                () =>
                {
                    BusyWait(TimeSpan.FromMilliseconds(2));
                    events.Enqueue($"actor-a:{index}");
                    return ValueTask.CompletedTask;
                }))
            .ToArray();
        var otherActor = ExecuteActorAsync(
            executor,
            "actor-b",
            () =>
            {
                events.Enqueue("actor-b");
                return ValueTask.CompletedTask;
            });
        releaseFirst.Set();

        await Task.WhenAll(remaining.Append(first).Append(otherActor))
            .WaitAsync(TestTimeout);
        var recorded = events.ToArray();
        Assert.True(
            Array.IndexOf(recorded, "actor-b")
            < Array.IndexOf(recorded, "actor-a:7"));
    }

    [Fact]
    public async Task SameGateAwaitedRequest_ThrowsImmediatelyAtThePublicCallSite()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);

        await executor.ExecuteAsync(
            async (_, _) =>
            {
                var call = new ZLinkInstanceSpotRequestCall<object>(
                    null!,
                    "test-spot",
                    new object());

                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                    await call.Async<object>());

                Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
            },
            CancellationToken.None);
    }

    private static ZLinkSpotSerialExecutor CreateSpotExecutor(
        IZLinkRuntimeFailureReporter errorSink,
        ZLinkUserSpotExecutionMode mode,
        ZLinkExecutionLanePolicy? spotLanePolicy = null,
        ZLinkExecutionLanePolicy? actorLanePolicy = null,
        ZLinkExecutionLanePolicy? timerLanePolicy = null)
    {
        return new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink,
            executionMode: mode,
            spotLanePolicy: spotLanePolicy,
            actorLanePolicy: actorLanePolicy,
            timerLanePolicy: timerLanePolicy);
    }

    private static ZLinkExecutionLanePolicy CreatePolicy(
        int applicationMessageCapacity = 32,
        long applicationByteCapacity = 1_048_576,
        int lifecycleMessageCapacity = 8,
        long lifecycleByteCapacity = 1_024,
        long fixedWorkByteCost = 1,
        int lifecycleBurstLimit = 8,
        TimeSpan? ownerTimeBudget = null)
    {
        return new ZLinkExecutionLanePolicy(
            applicationMessageCapacity,
            applicationByteCapacity,
            lifecycleMessageCapacity,
            lifecycleByteCapacity,
            fixedWorkByteCost,
            lifecycleBurstLimit,
            ownerTimeBudget ?? TimeSpan.FromSeconds(1));
    }

    private static Task ExecuteActorAsync(
        ZLinkSpotSerialExecutor executor,
        string actorId,
        Func<ValueTask> operation)
    {
        return executor.ExecuteActorAsync(
            actorId,
            static (_, callback, _) => callback(),
            operation,
            CancellationToken.None).AsTask();
    }

    private static Task ExecuteActorWithPayloadAsync(
        ZLinkSpotSerialExecutor executor,
        string actorId,
        Func<ValueTask> operation,
        long payloadBytes)
    {
        return executor.ExecuteActorAsync(
            actorId,
            static (_, callback, _) => callback(),
            operation,
            payloadBytes,
            metadataBytes: 0,
            transferred: false,
            CancellationToken.None).AsTask();
    }

    private static Task ExecuteTimerAsync(
        ZLinkSpotSerialExecutor executor,
        string timerName,
        Func<ValueTask> operation)
    {
        return executor.ExecuteTimerAsync(
            timerName,
            static (_, callback, _) => callback(),
            operation,
            CancellationToken.None).AsTask();
    }

    private static async Task AssertCapacityExceededAsync(
        Func<Task> operation)
    {
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(operation);
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, failure.Kind);
    }

    private static async Task AssertUpperQueueRejectsSecondActorAsync(
        long payloadBytes)
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateSpotExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide,
            spotLanePolicy: CreatePolicy(
                applicationMessageCapacity: 1,
                applicationByteCapacity: 2),
            actorLanePolicy: CreatePolicy(
                applicationMessageCapacity: 8,
                applicationByteCapacity: 1_048_576));
        var firstStarted = NewSignal();
        var releaseFirst = NewSignal();

        var first = ExecuteActorWithPayloadAsync(
            executor,
            "actor-a",
            async () =>
            {
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            payloadBytes: 1);
        await firstStarted.Task.WaitAsync(TestTimeout);

        await AssertCapacityExceededAsync(
            () => ExecuteActorWithPayloadAsync(
                executor,
                "actor-b",
                static () => ValueTask.CompletedTask,
                payloadBytes));

        releaseFirst.TrySetResult();
        await first.WaitAsync(TestTimeout);
    }

    private static void BusyWait(TimeSpan duration)
    {
        var startedAt = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(startedAt) < duration)
        {
        }
    }

    private static TaskCompletionSource NewSignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}
