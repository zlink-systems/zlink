using System.Collections.Concurrent;

namespace Zlink.Framework.UnitTests;

public sealed class WorkerPoolTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_Submit_BlocksUntilResult(bool io)
    {
        using var pool = CreatePool(1);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var call = TerminalCall(
            pool,
            io,
            async _ =>
            {
                entered.SetResult();
                return await release.Task;
            }
        );
        var submit = Task.Run(() => call.Submit());
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(await Task.WhenAny(submit, Task.Delay(100)) == submit);
        }
        finally
        {
            release.TrySetResult(42);
            await submit.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void WorkerTerminal_Submit_ThrowsWorkFailure(bool io)
    {
        using var pool = CreatePool(1);
        var cause = new InvalidOperationException("business failure");
        var call = TerminalCall(pool, io, _ => throw cause);
        var failure = Assert.Throws<ZLinkFrameworkException>(() => call.Submit());
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, failure.Kind);
        Assert.Same(cause, failure.InnerException);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_PreCanceled_DoesNotInvokeWork(bool io)
    {
        using var pool = CreatePool(1);
        using var stop = new CancellationTokenSource();
        stop.Cancel();
        var invoked = 0;
        var pending = TerminalCall(
                pool,
                io,
                _ =>
                {
                    Interlocked.Increment(ref invoked);
                    return ValueTask.FromResult(42);
                }
            )
            .Async(stop.Token)
            .AsTask();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        Assert.True(pending.IsCanceled);
        Assert.Equal(0, invoked);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task WorkerTerminal_BusinessTimeout_IsInternalFailure(
        bool asynchronous,
        bool deadline
    )
    {
        using var pool = CreatePool(1);
        var cause = new TimeoutException("business timeout");
        var call = TerminalCall(
            pool,
            true,
            _ => asynchronous ? ValueTask.FromException<int>(cause) : throw cause
        );
        if (deadline)
            call.Timeout(TimeSpan.FromSeconds(5));
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            call.Async().AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, failure.Kind);
        Assert.Same(cause, failure.InnerException);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_StoppedBeforeAdmission_IsShuttingDown(bool io)
    {
        using var pool = CreatePool(1);
        pool.RequestStop();
        var invoked = 0;
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            TerminalCall(
                    pool,
                    io,
                    _ =>
                    {
                        Interlocked.Increment(ref invoked);
                        return ValueTask.FromResult(42);
                    }
                )
                .Async()
                .AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
        Assert.Equal(0, invoked);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_RunningStop_IsShuttingDown(bool io)
    {
        using var pool = CreatePool(1);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var pending = TerminalCall(
                pool,
                io,
                async token =>
                {
                    entered.SetResult();
                    return await release.Task.WaitAsync(token);
                }
            )
            .Async()
            .AsTask();
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            pool.RequestStop();
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => pending);
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
        }
        finally
        {
            release.TrySetResult(42);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_RunningCallerCancellation_IsCanceled(bool io)
    {
        using var pool = CreatePool(1);
        using var stop = new CancellationTokenSource();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var pending = TerminalCall(
                pool,
                io,
                async token =>
                {
                    entered.SetResult();
                    return await release.Task.WaitAsync(token);
                }
            )
            .Async(stop.Token)
            .AsTask();
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            stop.Cancel();
            var failure = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
            Assert.Equal(stop.Token, failure.CancellationToken);
            Assert.True(pending.IsCanceled);
        }
        finally
        {
            release.TrySetResult(42);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_StateLaneSubmit_IsRejectedBeforeAdmission(bool io)
    {
        using var pool = CreatePool(1);
        var invoked = 0;
        var call = TerminalCall(pool, io, _ => ValueTask.FromResult(++invoked));
        await new ZLinkStateLane().RunAsync(() =>
        {
            var failure = Assert.Throws<ZLinkFrameworkException>(() => call.Submit());
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        });
        Assert.Equal(0, invoked);
        Assert.Equal(1, await call.Async());
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WorkerTerminal_CompletionDispatcherSubmit_IsRejectedBeforeAdmission(bool io)
    {
        using var pool = CreatePool(1);
        var invoked = 0;
        var call = TerminalCall(pool, io, _ => ValueTask.FromResult(++invoked));
        var check = new TerminalCompletionCheck(() =>
        {
            var failure = Assert.Throws<ZLinkFrameworkException>(() => call.Submit());
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        });
        ZLinkCompletionDispatcher.Post(check);
        await check.Finished.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, invoked);
        Assert.Equal(1, await call.Async());
    }

    [Fact]
    public async Task WorkerTerminal_IoDeadline_IsDeadlineExceeded_AndCancelsWork()
    {
        using var pool = CreatePool(1);
        var operation = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        CancellationToken workToken = default;
        var call = TerminalCall(
                pool,
                true,
                token =>
                {
                    workToken = token;
                    return new ValueTask<int>(operation.Task);
                }
            )
            .Timeout(TimeSpan.FromMilliseconds(50));
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            call.Async().AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
        Assert.True(workToken.IsCancellationRequested);
        operation.SetResult(42);
    }

    private static Zlink.Framework.Contracts.Workers.IZLinkWorkerCall<int> TerminalCall(
        ZLinkWorkerPool pool,
        bool io,
        Func<CancellationToken, ValueTask<int>> work
    )
    {
        return io
            ? new ZLinkIoWorkerCall<int>(pool.ShutdownToken, work)
            : new ZLinkWorkerCall<int>(
                pool,
                token => work(token).AsTask().GetAwaiter().GetResult()
            );
    }

    private sealed class TerminalCompletionCheck(Action action) : ZLinkCompletionDispatcher.WorkItem
    {
        internal readonly TaskCompletionSource Finished = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal override void Execute() => action();

        internal override void Completed(Exception? failure)
        {
            if (failure is null)
                Finished.TrySetResult();
            else
                Finished.TrySetException(failure);
        }
    }

    [Fact]
    public async Task UnexpectedWrapperFailure_ReachesRuntimeReporterAndKeepsPoolAlive()
    {
        using var sink = new ZLinkRuntimeErrorSink();
        var observed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        sink.UnhandledCallbackException += failure => observed.TrySetResult(failure);
        await using var pool = new ZLinkWorkerPool(0, 1, TimeSpan.FromSeconds(30), sink);
        var failure = new InvalidOperationException("worker wrapper failed");
        Assert.Equal(ZLinkWorkerSubmitResult.Accepted, pool.TrySubmit(_ => throw failure));
        Assert.Same(failure, await observed.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        var following = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.Equal(
            ZLinkWorkerSubmitResult.Accepted,
            pool.TrySubmit(_ => following.TrySetResult())
        );
        await following.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RunCpuWorker_Async_Holds_Serial_Turn_Until_Work_Completes()
    {
        using var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var releaseWork = new ManualResetEventSlim(false);
        var workerStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        var first = queue
            .RunAsync(
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
                        queue
                    );
                    _ = await call.Async();
                },
                CancellationToken.None
            )
            .AsTask();

        await workerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = queue
            .RunAsync(
                _ =>
                {
                    secondRan.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

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
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var firstResumed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        var first = queue
            .RunAsync(
                async cancellationToken =>
                {
                    _ = cancellationToken;
                    using var execution = ZLinkApplicationExecutionContext.Push(
                        new ZLinkApplicationExecutionScope(
                            "worker-test-spot",
                            ZLinkUserSpotExecutionMode.SpotWide,
                            ActorId: null,
                            YieldAllowed: true
                        )
                    );
                    var call = CreateCall(
                        pool,
                        _ =>
                        {
                            workerStarted.TrySetResult();
                            releaseWork.Wait();
                            return 1;
                        },
                        queue
                    );
                    _ = await call.Yield();
                    firstResumed.TrySetResult();
                },
                CancellationToken.None
            )
            .AsTask();

        await workerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = queue
            .RunAsync(
                _ =>
                {
                    secondRan.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

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
                queue
            )
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
    public async Task RunCpuWorker_QueueWaitsUntilTheBlockedWorkerCompletes()
    {
        using var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var blockPool = new ManualResetEventSlim(false);
        var workerStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        try
        {
            // Occupy the single pool thread, then queue more work than the
            // former worker-queue bound allowed.
            _ = CreateCall(
                    pool,
                    _ =>
                    {
                        workerStarted.TrySetResult();
                        blockPool.Wait();
                        return 0;
                    },
                    queue
                )
                .Async();
            await workerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            _ = CreateCall(pool, _ => 0, queue).Async();
            await WaitForAsync(() => pool.QueueLength == 1);

            var overflow = CreateCall(pool, _ => 3, queue).Async();
            Assert.False(overflow.IsCompleted);
            blockPool.Set();
            Assert.Equal(3, await overflow.AsTask().WaitAsync(TimeSpan.FromSeconds(5)));
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
            queue
        );
        call.Timeout(TimeSpan.FromMilliseconds(100));
        var pending = call.Async().AsTask();
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            pending.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, observed.Kind);

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
            new ZLinkRuntimeErrorSink()
        );
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

        var call = CreateCall<int>(pool, _ => throw new InvalidOperationException("boom"), queue);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await call.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5))
        );

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

        Assert.Equal(
            ZLinkWorkerSubmitResult.Accepted,
            pool.TrySubmit(_ =>
            {
                started.TrySetResult();
                release.Wait();
            })
        );
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var dispose = pool.DisposeAsync().AsTask();
        await Task.Delay(100);
        Assert.False(dispose.IsCompleted);

        release.Set();
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, pool.ThreadCount);
    }

    [Fact]
    public async Task RequestStop_Rejects_Queued_Work_AsShuttingDown()
    {
        var pool = CreatePool(1);
        await using var queue = CreateQueue();
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.Equal(
            ZLinkWorkerSubmitResult.Accepted,
            pool.TrySubmit(_ =>
            {
                started.TrySetResult();
                release.Wait();
            })
        );
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var queued = CreateCall(pool, _ => 42, queue).Async().AsTask();
        await WaitForAsync(() => pool.QueueLength == 1);

        pool.RequestStop();
        try
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => queued);
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
        }
        finally
        {
            release.Set();
            await pool.DisposeAsync();
        }
    }

    private static ZLinkWorkerPool CreatePool(int maxThreads)
    {
        return new ZLinkWorkerPool(
            0,
            maxThreads,
            TimeSpan.FromSeconds(30),
            new ZLinkRuntimeErrorSink()
        );
    }

    private static ZLinkWorkerCall<TResult> CreateCall<TResult>(
        ZLinkWorkerPool pool,
        Func<CancellationToken, TResult> work,
        ZLinkSerialExecutionQueue dispatcherQueue
    )
    {
        _ = dispatcherQueue;
        return new ZLinkWorkerCall<TResult>(pool, work);
    }

    private static ZLinkSerialExecutionQueue CreateQueue()
    {
        var errorSink = new ZLinkRuntimeErrorSink();
        return new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
    }

    private static async Task WaitForAsync(Func<bool> predicate, int timeoutMs = 5_000)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        while (!predicate())
        {
            if (Environment.TickCount64 > deadline)
                throw new TimeoutException("Condition was not reached in time.");

            await Task.Delay(10);
        }
    }
}
