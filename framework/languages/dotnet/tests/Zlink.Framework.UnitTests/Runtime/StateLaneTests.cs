using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// Specifies <see cref="ZLinkStateLane"/>, the ownership primitive every converted state component
/// relies on. Each test states one guarantee a component author is allowed to assume.
/// </summary>
public sealed class StateLaneTests
{
    [Fact]
    public async Task ContendingProducerAndSynchronousWorker_CompleteTurnsInRegisteredOrder()
    {
        await using var lane = new ZLinkStateLane();
        using var firstEntered = new ManualResetEventSlim();
        using var secondRegistered = new ManualResetEventSlim();
        var order = new List<int>();
        var active = 0;
        void Record(int value)
        {
            Assert.Equal(1, Interlocked.Increment(ref active));
            Assert.Same(lane, ZLinkStateLane.Current);
            order.Add(value);
            if (value == 1)
            {
                firstEntered.Set();
                Assert.True(secondRegistered.Wait(TimeSpan.FromSeconds(5)));
            }
            Assert.Equal(0, Interlocked.Decrement(ref active));
        }

        var producer = Task.Factory.StartNew(() =>
        {
            Assert.True(firstEntered.Wait(TimeSpan.FromSeconds(5)));
            var second = lane.RunAsync(() => Record(2));
            // Turn A remains held until this pending registration is observed.
            Assert.False(second.IsCompleted);
            secondRegistered.Set();
            second.AsTask().GetAwaiter().GetResult();
        }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        var worker = Task.Run(() =>
        {
            lane.RunAsync(() => Record(1)).GetAwaiter().GetResult();
            // A's release must schedule the already registered B. Immediately
            // submit C and synchronously wait on the same ThreadPool worker.
            lane.RunAsync(() => Record(3)).GetAwaiter().GetResult();
            Assert.Null(ZLinkStateLane.Current);
            Assert.Equal(new[] { 1, 2, 3 }, order);
        });

        await Task.WhenAll(worker, producer).WaitAsync(TimeSpan.FromSeconds(10));
        Assert.Equal(0, active);
    }

    [Fact]
    public async Task CallbackState_IsPreservedWhenTheTurnQueuesBehindAnAsyncTurn()
    {
        await using var lane = new ZLinkStateLane();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(lane.TryPost(async () =>
        {
            entered.SetResult();
            await release.Task;
        }));
        await entered.Task;
        var values = new int[1];
        try
        {
            var queued = lane.RunAsync((Values: values, Value: 42), static state =>
                state.Values[0] = state.Value);
            Assert.False(queued.IsCompleted);
            release.SetResult();
            Assert.Equal(42, await queued);
            Assert.Equal(42, values[0]);
        }
        finally
        {
            release.TrySetResult();
        }
    }

    [Fact]
    public async Task ConcurrentFirstPosts_ShareOneMailboxAndDrainEveryItem()
    {
        for (var iteration = 0; iteration < 16; iteration++)
        {
            var lane = new ZLinkStateLane();
            var seen = new int[64];
            await Task.WhenAll(Enumerable.Range(0, seen.Length).Select(index => Task.Run(() =>
                Assert.True(lane.TryPost(() =>
                {
                    seen[index]++;
                    return ValueTask.CompletedTask;
                })))));
            await lane.DisposeAsync();
            Assert.All(seen, count => Assert.Equal(1, count));
        }
    }

    [Fact]
    public async Task IdleSynchronousTurn_ReturnsValueWithoutATask()
    {
        await using var lane = new ZLinkStateLane();
        var result = lane.RunAsync(static () => 48);
        Assert.Equal(ValueTask.FromResult(48), result);
        Assert.Equal(48, await result);

        var completed = lane.RunAsync(static () => { });
        Assert.Equal(ValueTask.CompletedTask, completed);
        await completed;
    }

    [Fact]
    public async Task NestedIdleTurn_RestoresOuterOwnershipEvenWhenInnerWorkFails()
    {
        await using var outer = new ZLinkStateLane();
        await using var inner = new ZLinkStateLane();
        await outer.RunAsync(() =>
        {
            Assert.True(outer.IsOnLane);
            Assert.Throws<InvalidOperationException>(() => inner.RunAsync<int>(
                () => throw new InvalidOperationException("inner")).GetAwaiter().GetResult());
            Assert.Same(outer, ZLinkStateLane.Current);
            Assert.Throws<InvalidOperationException>(() => outer.RunAsync(static () => 0));
        });
        Assert.Null(ZLinkStateLane.Current);
    }

    // ---- 기본 동작 -------------------------------------------------------------------

    [Fact]
    public async Task RunAsync_ReturnsTheResultOfTheWork()
    {
        await using var lane = new ZLinkStateLane();

        var result = await lane.RunAsync(() => 42);

        Assert.Equal(42, result);
    }

    [Fact]
    public async Task RunAsync_SurfacesAFailureToItsOwnCaller()
    {
        await using var lane = new ZLinkStateLane();

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await lane.RunAsync<int>(
                () => throw new InvalidOperationException("boom")));
    }

    [Fact]
    public async Task RunAsync_KeepsServingAfterAWorkItemThrows()
    {
        await using var lane = new ZLinkStateLane();

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await lane.RunAsync<int>(() => throw new InvalidOperationException()));

        Assert.Equal(7, await lane.RunAsync(() => 7));
    }

    // ---- 소유권: 잠금 없이 안전한가 --------------------------------------------------

    [Fact]
    public async Task ConcurrentCallers_MutateUnsynchronizedStateWithoutLosingUpdates()
    {
        //  This is the whole point of the design: the dictionary is a plain Dictionary with no
        //  gate. If the lane did not serialize, this would corrupt or lose writes.
        await using var lane = new ZLinkStateLane();
        var state = new Dictionary<int, int>();
        const int callers = 32;
        const int perCaller = 50;

        await Task.WhenAll(Enumerable.Range(0, callers).Select(caller =>
            Task.Run(async () =>
            {
                for (var i = 0; i < perCaller; i++)
                {
                    var key = (caller * perCaller) + i;
                    await lane.RunAsync(() => state[key] = key);
                }
            })));

        Assert.Equal(callers * perCaller, await lane.RunAsync(() => state.Count));
    }

    [Fact]
    public async Task WorkItems_NeverOverlap()
    {
        await using var lane = new ZLinkStateLane();
        var inFlight = 0;
        var observedOverlap = false;

        await Task.WhenAll(Enumerable.Range(0, 64).Select(_ =>
            Task.Run(async () => await lane.RunAsync(() =>
            {
                if (Interlocked.Increment(ref inFlight) != 1)
                    observedOverlap = true;
                Thread.SpinWait(200);
                Interlocked.Decrement(ref inFlight);
                return 0;
            }))));

        Assert.False(observedOverlap);
    }

    [Fact]
    public async Task PostsFromOneCaller_RunInPostOrder()
    {
        await using var lane = new ZLinkStateLane();
        var order = new List<int>();

        for (var i = 0; i < 100; i++)
        {
            var value = i;
            Assert.True(lane.TryPost(() =>
            {
                order.Add(value);
                return ValueTask.CompletedTask;
            }));
        }

        Assert.Equal(Enumerable.Range(0, 100), await lane.RunAsync(() => order.ToArray()));
    }

    [Fact]
    public async Task DrainingMoreThanOneBatch_StillRunsEveryItem()
    {
        //  The drain yields after a bounded batch. Everything queued past that boundary has to be
        //  picked up by the reschedule, not dropped.
        await using var lane = new ZLinkStateLane();
        var count = 0;

        for (var i = 0; i < 250; i++)
            Assert.True(lane.TryPost(() => { count++; return ValueTask.CompletedTask; }));

        Assert.Equal(250, await lane.RunAsync(() => count));
    }

    [Fact]
    public async Task EnqueueRacingWithDrainRelease_DoesNotStrandTheNextTurn()
    {
        var lane = new ZLinkStateLane();
        var count = 0;
        // A single producer repeatedly races the completion of its previous
        // turn with the drainer's empty-queue check. Multiple producers tend to
        // keep the queue nonempty and hide this idle-transition race.
        var producer = Task.Run(() =>
        {
            for (var index = 0; index < 1_000_000; index++)
                lane.RunAsync(() => ++count).GetAwaiter().GetResult();
        });

        await producer.WaitAsync(TimeSpan.FromSeconds(30));
        Assert.Equal(1_000_000, count);
        await lane.DisposeAsync();
    }

    // ---- 재진입: 행 대신 진단 가능한 실패 ---------------------------------------------

    [Fact]
    public async Task ReenteringTheSameLane_FailsInsteadOfHanging()
    {
        await using var lane = new ZLinkStateLane();

        var error = await lane.RunAsync(() =>
            Assert.Throws<InvalidOperationException>(() => lane.RunAsync(() => 1)));

        Assert.Contains("already runs on the state lane", error.Message);
    }

    [Fact]
    public async Task IsOnLane_IsTrueOnlyInsideATurn()
    {
        await using var lane = new ZLinkStateLane();

        Assert.False(lane.IsOnLane);
        Assert.True(await lane.RunAsync(() => lane.IsOnLane));
        Assert.False(lane.IsOnLane);
    }

    [Fact]
    public async Task ADifferentLane_IsEnterableFromInsideATurn()
    {
        //  Reentrancy is per lane. Two components must still be able to call each other.
        await using var outer = new ZLinkStateLane();
        await using var inner = new ZLinkStateLane();

        var result = await outer.RunAsync(() => inner.RunAsync(() => 5).AsTask().Result);

        Assert.Equal(5, result);
    }

    [Fact]
    public async Task IdleSynchronousTurn_DrainsInlineAndRestoresCallerContext()
    {
        await using var lane = new ZLinkStateLane();
        var caller = Environment.CurrentManagedThreadId;
        var operation = lane.RunAsync(() =>
        {
            Assert.True(lane.IsOnLane);
            return Environment.CurrentManagedThreadId;
        });
        Assert.True(operation.IsCompletedSuccessfully);
        Assert.Equal(caller, await operation);
        Assert.Null(ZLinkStateLane.Current);
    }

    [Fact]
    public async Task InlineDrain_DoesNotPassAnEarlierSuspendedTurn()
    {
        await using var lane = new ZLinkStateLane();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<int>();
        lane.TryPost(async () =>
        {
            started.SetResult();
            await release.Task;
            order.Add(1);
        });
        await started.Task;
        var second = lane.RunAsync(() => order.Add(2));
        Assert.False(second.IsCompleted);
        release.SetResult();
        await second;
        Assert.Equal(new[] { 1, 2 }, order);
        Assert.Null(ZLinkStateLane.Current);
    }

    // ---- 종료 ------------------------------------------------------------------------

    [Fact]
    public async Task DisposeAsync_WaitsForQueuedWork()
    {
        var lane = new ZLinkStateLane();
        var completed = 0;

        for (var i = 0; i < 200; i++)
            lane.TryPost(() => { completed++; return ValueTask.CompletedTask; });

        await lane.DisposeAsync();

        Assert.Equal(200, completed);
    }

    [Fact]
    public async Task RunAsync_AfterDispose_Throws()
    {
        var lane = new ZLinkStateLane();
        await lane.DisposeAsync();

        Assert.Throws<ObjectDisposedException>(() => lane.RunAsync(() => 1));
    }

    [Fact]
    public async Task TryPost_AfterDispose_ReportsRefusalInsteadOfThrowing()
    {
        var lane = new ZLinkStateLane();
        await lane.DisposeAsync();

        Assert.False(lane.TryPost(() => ValueTask.CompletedTask));
    }

    [Fact]
    public async Task DisposeAsync_IsIdempotent()
    {
        var lane = new ZLinkStateLane();

        await lane.DisposeAsync();
        await lane.DisposeAsync();
    }
}
