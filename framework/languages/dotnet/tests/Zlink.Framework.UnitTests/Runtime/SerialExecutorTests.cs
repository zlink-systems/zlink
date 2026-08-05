using System.Collections.Concurrent;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class SerialExecutorTests
{
    [Fact]
    public async Task RelocationTimerReservationsShareAcceptedSequenceDomain()
    {
        await using var queue = CreateQueue(CancellationToken.None);

        Assert.True(queue.TrySealRelocation(
            reservedAcceptedSequences: 2,
            static _ => true,
            out var seal,
            out var firstTimerSequence));
        Assert.Equal<ulong>(1, firstTimerSequence);
        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[] { 3 },
            static _ => ValueTask.CompletedTask,
            static () => { },
            out _));

        Assert.True(queue.TryCommitRelocation(seal, out var held));
        Assert.Equal<ulong>(3, Assert.Single(held).AcceptedSequence);
    }

    [Fact]
    public async Task SerialExecutionQueue_RelocationSeal_CapturesPendingAndHoldsNewAcceptedWork()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var executionOrder = new ConcurrentQueue<int>();
        var relocated = new ConcurrentQueue<int>();

        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[] { 1 },
            async _ =>
            {
                activeStarted.TrySetResult();
                await releaseActive.Task.ConfigureAwait(false);
                executionOrder.Enqueue(1);
            },
            () => relocated.Enqueue(1),
            out _));
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[] { 2 },
            _ =>
            {
                executionOrder.Enqueue(2);
                return ValueTask.CompletedTask;
            },
            () => relocated.Enqueue(2),
            out _));
        var sealTask = queue.SealRelocationAsync(CancellationToken.None).AsTask();
        Assert.False(sealTask.IsCompleted);

        releaseActive.TrySetResult();
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Collection(
            seal.Captured,
            record =>
            {
                Assert.Equal<ulong>(2, record.AcceptedSequence);
                Assert.Equal(new byte[] { 2 }, record.Payload.ToArray());
            });

        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[] { 3 },
            _ =>
            {
                executionOrder.Enqueue(3);
                return ValueTask.CompletedTask;
            },
            () => relocated.Enqueue(3),
            out _));
        Assert.True(queue.TryAbortRelocation(seal));

        await WaitUntilAsync(
            () => executionOrder.Count == 3,
            TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { 1, 2, 3 }, executionOrder);
        Assert.Empty(relocated);
    }

    [Fact]
    public async Task Accepted_journal_factory_materializes_only_after_relocation_seal()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var started = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var materialized = 0;

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 1 },
                callback: async _ =>
                {
                    started.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                relocationRelease: static () => { },
                out _));

        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                payloadLength: 3,
                payloadFactory: () =>
                {
                    Interlocked.Increment(ref materialized);
                    return new byte[] { 7, 8, 9 };
                },
                callback: static _ => ValueTask.CompletedTask,
                relocationRelease: static () => { },
                previousOwnerMessageFollow: false,
                out _));
        Assert.Equal(0, Volatile.Read(ref materialized));

        var sealTask = queue.SealRelocationAsync(CancellationToken.None).AsTask();
        release.TrySetResult();
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(1, Volatile.Read(ref materialized));
        Assert.Equal(new byte[] { 7, 8, 9 }, Assert.Single(seal.Captured).Payload.ToArray());
        Assert.True(queue.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task Cancelled_async_relocation_seal_does_not_orphan_a_later_seal()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(queue.TryPost(
            async _ =>
            {
                activeStarted.TrySetResult();
                await releaseActive.Task.ConfigureAwait(false);
            },
            out var active));
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var cancellation = new CancellationTokenSource();
        var cancelledSeal = queue.SealRelocationAsync(
            cancellation.Token).AsTask();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => cancelledSeal);

        releaseActive.TrySetResult();
        await active.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        var nextRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(queue.TryPost(
            _ =>
            {
                nextRan.TrySetResult();
                return ValueTask.CompletedTask;
            },
            out _));
        await nextRan.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var seal = await queue.SealRelocationAsync(
            CancellationToken.None);
        Assert.True(queue.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task Async_seal_reserves_timer_sequence_at_boundary_before_held_ingress()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(queue.TryPost(
            async _ =>
            {
                activeStarted.TrySetResult();
                await releaseActive.Task.ConfigureAwait(false);
            },
            out _));
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var boundaryReached = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var sealTask = queue.SealRelocationAsync(
            () =>
            {
                boundaryReached.TrySetResult();
                return 1;
            },
            CancellationToken.None).AsTask();
        releaseActive.TrySetResult();
        await boundaryReached.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal<ulong>(1, seal.FirstReservedSequence);
        Assert.Equal(1, seal.ReservedAcceptedSequences);

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 9 },
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _));
        Assert.True(queue.TryCommitRelocation(seal, out var held));
        Assert.Equal<ulong>(2, Assert.Single(held).AcceptedSequence);
    }

    [Fact]
    public async Task TargetCutoverRunsPreviousOwnerMessageFollowBeforeDirectIngress()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        var executionOrder = new ConcurrentQueue<int>();

        ZLinkSerialWorkItem Post(int value, bool followed)
        {
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[] { (byte)value },
                    _ =>
                    {
                        executionOrder.Enqueue(value);
                        return ValueTask.CompletedTask;
                    },
                    static () => { },
                    followed,
                    out var item));
            return item;
        }

        var directFirst = Post(1, followed: false);
        var followed = Post(2, followed: true);
        var directSecond = Post(3, followed: false);

        Assert.True(queue.TryOpenRelocationAfterMessageFollow(seal));
        await Task.WhenAll(
            directFirst.Completion,
            followed.Completion,
            directSecond.Completion).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { 2, 1, 3 }, executionOrder);
    }

    [Fact]
    public async Task SerialExecutionQueue_RelocationCommit_ReleasesCapturedAndReturnsHeldRecords()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        var released = new ConcurrentQueue<int>();

        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[] { 7 },
            static _ => throw new InvalidOperationException("held work must not execute"),
            () => released.Enqueue(7),
            out var heldItem));

        Assert.True(queue.TryCommitRelocation(seal, out var held));
        await heldItem.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Collection(
            held,
            record =>
            {
                Assert.Equal<ulong>(1, record.AcceptedSequence);
                Assert.Equal(new byte[] { 7 }, record.Payload.ToArray());
            });
        Assert.Equal(new[] { 7 }, released);
        Assert.Equal(ZLinkAcceptedWorkAdmission.RelocationMoving, queue.TryPostAccepted(
            new byte[] { 8 },
            static _ => ValueTask.CompletedTask,
            static () => { },
            out _));
    }

    [Fact]
    public async Task RelocationIngressHoldRemainsOpenUntilCommitAndIsBounded()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        for (var index = 0; index < 1_024; index++)
            Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
                new byte[] { 7 },
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _));
        Assert.Equal(ZLinkAcceptedWorkAdmission.RelocationMoving, queue.TryPostAccepted(
            new byte[] { 8 },
            static _ => ValueTask.CompletedTask,
            static () => { },
            out _));

        Assert.True(queue.TryCommitRelocation(seal, out var held));
        Assert.Equal(1_024, held.Count);
    }

    [Fact]
    public async Task RelocationIngressHoldRejectsPayloadPastByteBound()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[16 * 1024 * 1024 - sizeof(ulong) - sizeof(int)],
            static _ => ValueTask.CompletedTask,
            static () => { },
            out _));
        Assert.Equal(ZLinkAcceptedWorkAdmission.RelocationMoving, queue.TryPostAccepted(
            new byte[] { 1 },
            static _ => ValueTask.CompletedTask,
            static () => { },
            out _));

        Assert.True(queue.TryCommitRelocation(seal, out var held));
        Assert.Single(held);
    }

    [Fact]
    public async Task SerialExecutionQueue_Dispose_AbortsUnfinishedRelocationWithoutHanging()
    {
        var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        var executed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, queue.TryPostAccepted(
            new byte[] { 9 },
            _ =>
            {
                executed.TrySetResult();
                return ValueTask.CompletedTask;
            },
            static () => { },
            out _));

        await queue.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await executed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(queue.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task SerialExecutionQueue_Dispose_Joins_The_Drain_Epilogue_Before_Disposing_Its_Gate()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            for (var iteration = 0; iteration < 100; iteration++)
            {
                var runner = new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None);
                var queue = new ZLinkSerialExecutionQueue(
                    runner,
                    errorSink,
                    CancellationToken.None);
                Assert.True(queue.TryPost(
                    static _ => ValueTask.CompletedTask,
                    out var item));

                await item.Completion;
                await queue.DisposeAsync();
                await runner.StopAsync();
            }

            Assert.DoesNotContain(
                exceptions,
                static exception => exception is ObjectDisposedException);
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task StreamSessionSerialExecutor_Continues_After_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var executor = new ZLinkStreamSessionSerialExecutor(new object(), errorSink);
            var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            Assert.True(executor.EnqueueInfrastructure(() => throw new InvalidOperationException("stream failure")));
            Assert.True(executor.EnqueueInfrastructure(() =>
            {
                completed.SetResult();
                return ValueTask.CompletedTask;
            }));

            await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains(exceptions, static ex => ex.Message == "stream failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_FinalTurn_BypassesCapacity_AndSealsAdmission()
    {
        await using var queue = CreateQueue(CancellationToken.None, capacity: 1);
        var firstStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var finalRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async _ =>
            {
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            out _));
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(queue.TryPost(static _ => ValueTask.CompletedTask, out _));
        Assert.True(queue.TryPostFinal(
            _ =>
            {
                finalRan.TrySetResult();
                return ValueTask.CompletedTask;
            },
            out _));
        Assert.False(queue.TryPost(static _ => ValueTask.CompletedTask, out _));
        Assert.False(queue.TryPostFinal(static _ => ValueTask.CompletedTask, out _));

        releaseFirst.TrySetResult();
        await finalRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_Cancellation_Joins_NonCooperative_Callback()
    {
        using var stop = new CancellationTokenSource();
        await using var queue = CreateQueue(stop.Token);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async _ =>
            {
                started.TrySetResult();
                await release.Task.ConfigureAwait(false);
            },
            out _));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        stop.Cancel();
        queue.Complete();
        var disposeTask = queue.DisposeAsync().AsTask();
        await Task.Delay(50);
        Assert.False(disposeTask.IsCompleted);

        release.TrySetResult();
        await disposeTask.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task SerialExecutionQueue_Cancellation_Joins_Cooperative_Callback()
    {
        using var stop = new CancellationTokenSource();
        await using var queue = CreateQueue(stop.Token);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cancellationObserved = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async cancellationToken =>
            {
                started.TrySetResult();
                try
                {
                    await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    cancellationObserved.TrySetResult();
                    throw;
                }
            },
            out _));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        stop.Cancel();
        queue.Complete();
        await queue.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        await cancellationObserved.Task.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task SerialExecutionQueue_Suspended_Cancellation_Joins_Callback()
    {
        using var stop = new CancellationTokenSource();
        await using var queue = CreateQueue(stop.Token);
        var operationStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var operation = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cancellationObserved = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCallback = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async cancellationToken =>
            {
                var turn = ZLinkSerialTurn.Current
                           ?? throw new InvalidOperationException("serial turn was not available");
                try
                {
                    await turn.YieldFrameworkCallAsync(
                            async _ =>
                            {
                                operationStarted.TrySetResult();
                                await operation.Task.ConfigureAwait(false);
                            },
                            cancellationToken)
                        .ConfigureAwait(false);
                }
                catch when (cancellationToken.IsCancellationRequested)
                {
                    cancellationObserved.TrySetResult();
                    await releaseCallback.Task.ConfigureAwait(false);
                }
            },
            out _));
        await operationStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        stop.Cancel();
        queue.Complete();
        var disposeTask = queue.DisposeAsync().AsTask();
        await cancellationObserved.Task.WaitAsync(TimeSpan.FromSeconds(1));
        await Task.Delay(50);
        Assert.False(disposeTask.IsCompleted);

        releaseCallback.TrySetResult();
        await disposeTask.WaitAsync(TimeSpan.FromSeconds(1));
        operation.TrySetResult();
    }

    [Fact]
    public async Task SpotSerialExecutor_Continues_After_Queued_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var executor = new ZLinkSpotSerialExecutor(
                null!,
                static () => false,
                CancellationToken.None,
                errorSink);
            var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            executor.Queue(static (_, _) => throw new InvalidOperationException("spot failure"));
            executor.Queue((_, _) =>
            {
                completed.SetResult();
                return ValueTask.CompletedTask;
            });

            await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains(exceptions, static ex => ex.Message == "spot failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SpotSerialExecutor_Queue_Reports_Skipped_Work_When_Stopped()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink);
        var skipped = 0;

        executor.RequestStop();

        Assert.False(executor.Queue(
            static (_, _) => ValueTask.CompletedTask,
            () => skipped++));
        Assert.False(executor.QueueNext(
            static (_, _) => ValueTask.CompletedTask,
            () => skipped++));
        Assert.Equal(2, skipped);
    }

    [Fact]
    public async Task SpotSerialExecutor_ExecuteAsync_Propagates_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var executor = new ZLinkSpotSerialExecutor(
                null!,
                static () => false,
                CancellationToken.None,
                errorSink);

            var thrown = await Assert.ThrowsAsync<InvalidOperationException>(() => executor.ExecuteAsync(
                static (_, _) => throw new InvalidOperationException("spot execute failure"),
                CancellationToken.None).AsTask());

            Assert.Equal("spot execute failure", thrown.Message);
            Assert.Contains(exceptions, static ex => ex.Message == "spot execute failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SpotSerialExecutor_LifecycleCallback_Uses_ApplicationLane_For_ApplicationCode()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink);
        var applicationRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var beforeApplicationCallback = executor.LastApplicationWorkCompletedAt;
        await Task.Delay(10);

        var lifecycle = executor.ExecuteLifecycleAsync(
            async (_, cancellationToken) =>
            {
                await executor.ExecuteApplicationCallbackAsync(
                        static (_, completed, _) =>
                        {
                            Assert.True(
                                ZLinkApplicationExecutionContext.Current
                                    is { YieldAllowed: true });
                            Assert.NotNull(ZLinkSerialTurn.Current);
                            completed.TrySetResult();
                            return ValueTask.CompletedTask;
                        },
                        applicationRan,
                        cancellationToken)
                    .ConfigureAwait(false);
            },
            CancellationToken.None)
            .AsTask();

        await applicationRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await lifecycle.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(
            executor.LastApplicationWorkCompletedAt > beforeApplicationCallback);
    }

    [Fact]
    public async Task SerialExecutionQueue_RunAsync_Propagates_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var queue = CreateQueue(CancellationToken.None, errorSink: errorSink);

            var thrown = await Assert.ThrowsAsync<InvalidOperationException>(() => queue.RunAsync(
                static _ => throw new InvalidOperationException("queue failure"),
                CancellationToken.None).AsTask());

            Assert.Equal("queue failure", thrown.Message);
            Assert.Contains(exceptions, static ex => ex.Message == "queue failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_Wait_Cancellation_Does_Not_Remove_Queued_Work()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var secondRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var first = queue.RunAsync(
            async _ =>
            {
                firstStarted.SetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var waitCancellation = new CancellationTokenSource();
        var second = queue.RunAsync(
            _ =>
            {
                secondRan.SetResult();
                return ValueTask.CompletedTask;
            },
            waitCancellation.Token).AsTask();
        waitCancellation.Cancel();

        try
        {
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => second.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.False(secondRan.Task.IsCompleted);
        }
        finally
        {
            releaseFirst.SetResult();
        }

        await first.WaitAsync(TimeSpan.FromSeconds(5));
        await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_TryPost_ReturnsFalse_WhenQueueIsFull()
    {
        await using var queue = CreateQueue(CancellationToken.None, 1);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async _ => await releaseFirst.Task.ConfigureAwait(false),
            out _));
        Assert.False(queue.TryPost(
            _ => ValueTask.CompletedTask,
            out _));

        releaseFirst.SetResult();
    }

    [Fact]
    public async Task SerialExecutionQueue_Distinguishes_QueueFull_From_Closed()
    {
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(
                new ZLinkRuntimeErrorSink(),
                CancellationToken.None),
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            capacity: 1);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async _ => await releaseFirst.Task.ConfigureAwait(false),
            out _));
        Assert.Equal(
            ZLinkSerialPostAdmission.QueueFull,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                out _));
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostNextWithAdmission(
                static _ => ValueTask.CompletedTask,
                out _));

        releaseFirst.TrySetResult();
        queue.Complete();
        Assert.Equal(
            ZLinkSerialPostAdmission.Closed,
            queue.TryPostNextWithAdmission(
                static _ => ValueTask.CompletedTask,
                out _));
    }

    [Fact]
    public async Task SerialExecutionQueue_Reserves_Count_And_Bytes_Per_Lane()
    {
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(
                new ZLinkRuntimeErrorSink(),
                CancellationToken.None),
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            capacity: 8,
            applicationByteCapacity:
                ZLinkSerialExecutionQueue.WorkItemFixedCostBytes + 4,
            lifecycleByteCapacity:
                ZLinkSerialExecutionQueue.WorkItemFixedCostBytes);

        var release = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 1, 2, 3, 4 },
                async _ => await release.Task.ConfigureAwait(false),
                static () => { },
                out var accepted));
        Assert.Equal(
            ZLinkSerialPostAdmission.QueueFull,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                out _));

        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostNextWithAdmission(
                static _ => ValueTask.CompletedTask,
                out var lifecycle));

        release.TrySetResult();
        await Task.WhenAll(accepted.Completion, lifecycle.Completion)
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                ReadOnlyMemory<byte>.Empty,
                static _ => ValueTask.CompletedTask,
                static () => { },
                out var next));
        await next.Completion.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_Selects_Lifecycle_Before_Ready_Application()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();

        Assert.True(queue.TryPost(
            async _ =>
            {
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
                order.Enqueue("first");
            },
            out var first));
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(queue.TryPostApplication(
            _ =>
            {
                order.Enqueue("application");
                return ValueTask.CompletedTask;
            },
            out var application));
        Assert.True(queue.TryPostNext(
            _ =>
            {
                order.Enqueue("lifecycle");
                return ValueTask.CompletedTask;
            },
            out var lifecycle));

        releaseFirst.TrySetResult();
        await Task.WhenAll(first.Completion, application.Completion, lifecycle.Completion)
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "first", "lifecycle", "application" }, order);
    }

    [Fact]
    public async Task SerialExecutionQueue_YieldDebt_Prevents_Lifecycle_Starvation_Of_Application()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();

        Assert.True(queue.TryPostNext(
            async _ =>
            {
                firstStarted.TrySetResult();
                await releaseFirst.Task.ConfigureAwait(false);
                order.Enqueue("lifecycle-0");
            },
            out var first));
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var lifecycle = new List<ZLinkSerialWorkItem> { first };
        for (var index = 1; index < ZLinkSerialExecutionQueue.LifecycleTurnLimit + 8; index++)
        {
            Assert.True(queue.TryPostNext(
                _ =>
                {
                    order.Enqueue($"lifecycle-{index}");
                    return ValueTask.CompletedTask;
                },
                out var item));
            lifecycle.Add(item);
        }

        Assert.True(queue.TryPostApplication(
            _ =>
            {
                order.Enqueue("application");
                return ValueTask.CompletedTask;
            },
            out var application));

        releaseFirst.TrySetResult();
        await Task.WhenAll(lifecycle.Select(static item => item.Completion)
                .Append(application.Completion))
            .WaitAsync(TimeSpan.FromSeconds(5));

        var applicationIndex = order.ToArray().ToList().IndexOf("application");
        Assert.InRange(
            applicationIndex,
            0,
            ZLinkSerialExecutionQueue.LifecycleTurnLimit);
    }

    [Fact]
    public async Task SerialExecutionQueue_PostAsync_Throws_WhenQueueIsFull()
    {
        await using var queue = CreateQueue(CancellationToken.None, 1);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(queue.TryPost(
            async _ => await releaseFirst.Task.ConfigureAwait(false),
            out _));

        var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => queue.PostAsync(
            _ => ValueTask.CompletedTask,
            CancellationToken.None).AsTask());
        Assert.Equal(
            ZLinkFrameworkErrorKind.CapacityExceeded,
            exception.Kind);

        releaseFirst.SetResult();
    }

    [Fact]
    public async Task SerialExecutionQueue_DefaultAwait_Holds_Gate_Until_Work_Completes()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var secondRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var first = queue.RunAsync(
            async _ =>
            {
                firstStarted.SetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var second = queue.RunAsync(
            _ =>
            {
                secondRan.SetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        await Task.Delay(100);
        Assert.False(secondRan.Task.IsCompleted);

        releaseFirst.SetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Allows_Later_Work_Then_Resumes_On_Line()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var ioStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var completeIo = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var firstResumed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstResume = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var thirdRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();

        var first = queue.RunAsync(
            async ct =>
            {
                order.Enqueue("first-start");
                var turn = ZLinkSerialTurn.Current
                           ?? throw new InvalidOperationException("serial turn was not available");
                await turn.YieldFrameworkCallAsync(
                    async _ =>
                    {
                        ioStarted.SetResult();
                        await completeIo.Task.ConfigureAwait(false);
                    },
                    ct).ConfigureAwait(false);
                order.Enqueue("first-resumed");
                firstResumed.SetResult();
                await releaseFirstResume.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();

        await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await queue.RunAsync(
            _ =>
            {
                order.Enqueue("second");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "first-start", "second" }, order.ToArray());

        completeIo.SetResult();
        await firstResumed.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var third = queue.RunAsync(
            _ =>
            {
                order.Enqueue("third");
                thirdRan.SetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        await Task.Delay(100);
        Assert.False(thirdRan.Task.IsCompleted);

        releaseFirstResume.SetResult();
        await Task.WhenAll(first, third).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { "first-start", "second", "first-resumed", "third" }, order.ToArray());
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Fault_Cleans_Pending_Turn()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var queue = CreateQueue(CancellationToken.None, errorSink: errorSink);
            var ioStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var failIo = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var secondRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var thirdRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            var first = queue.RunAsync(
                async ct =>
                {
                    var turn = ZLinkSerialTurn.Current
                               ?? throw new InvalidOperationException("serial turn was not available");
                    await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            ioStarted.SetResult();
                            await failIo.Task.ConfigureAwait(false);
                        },
                        ct).ConfigureAwait(false);
                },
                CancellationToken.None).AsTask();

            await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            await queue.RunAsync(
                _ =>
                {
                    secondRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

            failIo.SetException(new InvalidOperationException("yield I/O failed"));

            var thrown =
                await Assert.ThrowsAsync<InvalidOperationException>(() => first.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.Equal("yield I/O failed", thrown.Message);

            await queue.RunAsync(
                _ =>
                {
                    thirdRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

            await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await thirdRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains(exceptions, static ex => ex.Message == "yield I/O failed");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Cancellation_Cleans_Pending_Turn()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var queue = CreateQueue(CancellationToken.None, errorSink: errorSink);
            var ioStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var cancelIo = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var secondRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var thirdRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            var first = queue.RunAsync(
                async ct =>
                {
                    var turn = ZLinkSerialTurn.Current
                               ?? throw new InvalidOperationException("serial turn was not available");
                    await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            ioStarted.SetResult();
                            await cancelIo.Task.ConfigureAwait(false);
                        },
                        ct).ConfigureAwait(false);
                },
                CancellationToken.None).AsTask();

            await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            await queue.RunAsync(
                _ =>
                {
                    secondRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

            cancelIo.SetCanceled();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first.WaitAsync(TimeSpan.FromSeconds(5)));

            await queue.RunAsync(
                _ =>
                {
                    thirdRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

            await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await thirdRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.DoesNotContain(exceptions, static ex => ex is OperationCanceledException);
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Caller_Cancellation_Allows_Later_Work_Before_Operation_Completes()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        using var cancelWait = new CancellationTokenSource();
        var ioStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var completeIo = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var secondRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var thirdRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();

        var first = queue.RunAsync(
            async ct =>
            {
                var turn = ZLinkSerialTurn.Current
                           ?? throw new InvalidOperationException("serial turn was not available");
                try
                {
                    await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            ioStarted.SetResult();
                            await completeIo.Task.ConfigureAwait(false);
                        },
                        cancelWait.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    order.Enqueue("first-cancelled");
                }
            },
            CancellationToken.None).AsTask();

        await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await queue.RunAsync(
            _ =>
            {
                order.Enqueue("second");
                secondRan.SetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        await cancelWait.CancelAsync();
        await first.WaitAsync(TimeSpan.FromSeconds(5));

        await queue.RunAsync(
            _ =>
            {
                order.Enqueue("third");
                thirdRan.SetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(completeIo.Task.IsCompleted);
        completeIo.SetResult();
        await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await thirdRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { "second", "first-cancelled", "third" }, order.ToArray());
    }

    [Fact]
    public async Task ActorDispatchCancellation_Does_Not_Stop_Current_Or_Later_Dispatch()
    {
        var state = new ZLinkActorRuntimeState("test-actor");
        var firstStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var secondRan = false;
        var thirdRan = false;

        var first = state.ExecuteDispatchAsync(
            CreateHeader("first"),
            async _ =>
            {
                firstStarted.SetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var secondCancellation = new CancellationTokenSource();
        var second = state.ExecuteDispatchAsync(
            CreateHeader("second"),
            _ =>
            {
                secondRan = true;
                return ValueTask.CompletedTask;
            },
            secondCancellation.Token).AsTask();
        await secondCancellation.CancelAsync();

        await Assert.ThrowsAsync<OperationCanceledException>(() => second.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.False(secondRan);

        releaseFirst.SetResult();
        await first.WaitAsync(TimeSpan.FromSeconds(5));

        await state.ExecuteDispatchAsync(
            CreateHeader("third"),
            _ =>
            {
                thirdRan = true;
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(thirdRan);
    }

    [Fact]
    public async Task ActorDispatchMailbox_Runs_Waiters_In_Fifo_Order()
    {
        var state = new ZLinkActorRuntimeState("test-actor");
        var firstStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<string>();

        var first = state.ExecuteDispatchAsync(
            CreateHeader("first"),
            async _ =>
            {
                order.Add("first");
                firstStarted.SetResult();
                await releaseFirst.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var second = state.ExecuteDispatchAsync(
            CreateHeader("second"),
            _ =>
            {
                order.Add("second");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();
        var third = state.ExecuteDispatchAsync(
            CreateHeader("third"),
            _ =>
            {
                order.Add("third");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        releaseFirst.SetResult();

        await Task.WhenAll(first, second, third).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(["first", "second", "third"], order);
    }

    private static ZlinkStreamHeader CreateHeader(string name)
    {
        return new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            name,
            ZlinkStreamMetadata.Empty);
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition,
        TimeSpan timeout)
    {
        using var cancellation = new CancellationTokenSource(timeout);
        while (!condition())
            await Task.Delay(10, cancellation.Token);
    }

    private static ZLinkSerialExecutionQueue CreateQueue(
        CancellationToken executionToken,
        int capacity = 4096,
        ZLinkRuntimeErrorSink? errorSink = null)
    {
        errorSink ??= new ZLinkRuntimeErrorSink();
        return new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, executionToken),
            errorSink,
            executionToken,
            capacity);
    }
}
