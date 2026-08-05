using System.Collections.Concurrent;

namespace Zlink.Framework.UnitTests;

public sealed class WorkerPoolTests
{
    [Fact]
    public async Task RunCpuWorker_Async_Holds_Serial_Turn_Until_Work_Completes()
    {
        using var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var releaseWork = new ManualResetEventSlim(false);
        var workerStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var first = queue.RunAsync(
            async cancellationToken =>
            {
                _ = cancellationToken;
                var call = CreateCall(
                    pool,
                    _ =>
                    {
                        workerStarted.TrySetResult();
                        releaseWork.Wait();
                        return 1;
                    },
                    queue);
                _ = await call.Async();
            },
            CancellationToken.None).AsTask();

        await workerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = queue.RunAsync(
            _ =>
            {
                secondRan.TrySetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        try
        {
            await Task.Delay(100);
            Assert.False(secondRan.Task.IsCompleted);
        }
        finally
        {
            releaseWork.Set();
        }

        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RunCpuWorker_Yield_Releases_And_Resumes_Through_Serial_Turn()
    {
        using var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var releaseWork = new ManualResetEventSlim(false);
        var workerStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var firstResumed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var first = queue.RunAsync(
            async cancellationToken =>
            {
                _ = cancellationToken;
                using var execution = ZLinkApplicationExecutionContext.Push(
                    new ZLinkApplicationExecutionScope(
                        "worker-test-spot",
                        ZLinkUserSpotExecutionMode.SpotWide,
                        ActorId: null,
                        YieldAllowed: true));
                var call = CreateCall(
                    pool,
                    _ =>
                    {
                        workerStarted.TrySetResult();
                        releaseWork.Wait();
                        return 1;
                    },
                    queue);
                _ = await call.Yield();
                firstResumed.TrySetResult();
            },
            CancellationToken.None).AsTask();

        await workerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = queue.RunAsync(
            _ =>
            {
                secondRan.TrySetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        try
        {
            await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(firstResumed.Task.IsCompleted);
        }
        finally
        {
            releaseWork.Set();
        }

        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        await firstResumed.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RunCpuWorker_Async_Returns_Result_From_Pool_Thread()
    {
        using var pool = CreatePool(2);
        await using var queue = CreateQueue();

        var call = CreateCall(pool, _ => Environment.CurrentManagedThreadId, queue);
        var workerThreadId = await call.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        Assert.NotEqual(Environment.CurrentManagedThreadId, workerThreadId);
    }

    [Fact]
    public async Task RunCpuWorker_PreCanceledToken_DoesNotAdmitWork_AndReturnsCanceledTask()
    {
        using var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var sideEffects = 0;

        var pending = CreateCall(
                pool,
                _ =>
                {
                    Interlocked.Increment(ref sideEffects);
                    return 1;
                },
                queue)
            .Async(cancellation.Token)
            .AsTask();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        await Task.Delay(100);

        Assert.True(pending.IsCanceled);
        Assert.Equal(0, Volatile.Read(ref sideEffects));
        Assert.Equal(0, pool.QueueLength);
        Assert.Equal(0, pool.ThreadCount);
    }

    [Fact]
    public async Task RunCpuWorker_Queue_Full_Fails_Fast_With_WorkerQueueFull()
    {
        using var pool = CreatePool(1, 1);
        await using var queue = CreateQueue();
        using var blockPool = new ManualResetEventSlim(false);
        var workerStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        try
        {
            // Occupy the single pool thread, then fill the single queue slot.
            _ = CreateCall(
                pool,
                _ =>
                {
                    workerStarted.TrySetResult();
                    blockPool.Wait();
                    return 0;
                },
                queue).Async();
            await workerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            _ = CreateCall(pool, _ => 0, queue).Async();
            await WaitForAsync(() => pool.QueueLength == 1);

            var overflow = CreateCall(pool, _ => 0, queue).Async();
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await overflow.AsTask().WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, error.Kind);
            Assert.True(error.RetryAdvice != ZLinkRetryAdvice.DoNotRetry);
        }
        finally
        {
            blockPool.Set();
        }
    }

    [Fact]
    public async Task RunCpuWorker_Timeout_Drops_Late_Completion()
    {
        using var pool = CreatePool(2);
        await using var queue = CreateQueue();
        using var releaseWork = new ManualResetEventSlim(false);
        var call = CreateCall(
            pool,
            _ =>
            {
                releaseWork.Wait();
                return 7;
            },
            queue);
        call.Timeout(TimeSpan.FromMilliseconds(100));
        var pending = call.Async().AsTask();
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            pending.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            observed.Kind);

        // Let the abandoned work finish; its late completion must not change
        // the already completed timeout result.
        releaseWork.Set();
        await Task.Delay(300);
        Assert.True(pending.IsFaulted);
    }

    [Fact]
    public async Task RunCpuWorker_Idle_Threads_Shrink_After_Idle_Timeout()
    {
        using var pool = new ZLinkWorkerPool(
            0,
            2,
            TimeSpan.FromMilliseconds(150),
            16);
        await using var queue = CreateQueue();

        await CreateCall(pool, _ => 1, queue).Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(pool.ThreadCount >= 1);

        await WaitForAsync(() => pool.ThreadCount == 0);
        Assert.Equal(0, pool.ThreadCount);
    }

    [Fact]
    public async Task RunCpuWorker_Worker_Exception_Maps_To_WorkerFailed()
    {
        using var pool = CreatePool(2);
        await using var queue = CreateQueue();

        var call = CreateCall<int>(
            pool,
            _ => throw new InvalidOperationException("boom"),
            queue);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await call.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5)));

        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, error.Kind);
        Assert.False(error.RetryAdvice != ZLinkRetryAdvice.DoNotRetry);
        Assert.IsType<InvalidOperationException>(error.InnerException);
    }

    [Fact]
    public async Task RunCpuWorker_Second_Terminator_Throws()
    {
        using var pool = CreatePool(2);
        await using var queue = CreateQueue();

        var call = CreateCall(pool, _ => 1, queue);
        _ = call.Async();
        Assert.Throws<InvalidOperationException>(() => call.Async());
    }

    [Fact]
    public async Task Dispose_Waits_For_The_Running_Generation_Worker()
    {
        var pool = CreatePool(1);
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.Equal(ZLinkWorkerSubmitResult.Accepted, pool.TrySubmit(_ =>
        {
            started.TrySetResult();
            release.Wait();
        }));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var dispose = pool.DisposeAsync().AsTask();
        await Task.Delay(100);
        Assert.False(dispose.IsCompleted);

        release.Set();
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, pool.ThreadCount);
    }

    [Fact]
    public async Task RequestStop_Cancels_Work_That_Has_Not_Started()
    {
        var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.Equal(ZLinkWorkerSubmitResult.Accepted, pool.TrySubmit(_ =>
        {
            started.TrySetResult();
            release.Wait();
        }));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var queued = CreateCall(pool, _ => 42, queue).Async().AsTask();
        await WaitForAsync(() => pool.QueueLength == 1);

        pool.RequestStop();
        await Assert.ThrowsAsync<OperationCanceledException>(() => queued);

        release.Set();
        await pool.DisposeAsync();
    }

    private static ZLinkWorkerPool CreatePool(
        int maxThreads,
        int maxQueueLength = 16)
    {
        return new ZLinkWorkerPool(
            0,
            maxThreads,
            TimeSpan.FromSeconds(30),
            maxQueueLength);
    }

    private static ZLinkWorkerCall<TResult> CreateCall<TResult>(
        ZLinkWorkerPool pool,
        Func<CancellationToken, TResult> work,
        ZLinkSerialExecutionQueue dispatcherQueue)
    {
        _ = dispatcherQueue;
        return new ZLinkWorkerCall<TResult>(pool, work, new ZLinkRuntimeErrorSink());
    }

    private static ZLinkSerialExecutionQueue CreateQueue()
    {
        var errorSink = new ZLinkRuntimeErrorSink();
        return new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None);
    }

    private static async Task WaitForAsync(Func<bool> predicate, int timeoutMs = 5_000)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        while (!predicate())
        {
            if (Environment.TickCount64 > deadline) throw new TimeoutException("Condition was not reached in time.");

            await Task.Delay(10);
        }
    }
}
