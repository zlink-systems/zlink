using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

public sealed class MeshCompletionTableConvergenceTests
{
    [Fact]
    public async Task LateSendAdmissionUsesTheRegisteredDispatcherNode()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(92, 1);
        var pending = table.RegisterSend(operation);
        var admission = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        pending.Observe(admission.Task);
        var turn = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var awaiter = pending.Completion.ConfigureAwait(false).GetAwaiter();
        awaiter.UnsafeOnCompleted(() =>
        {
            awaiter.GetResult();
            turn.SetResult(ZLinkCompletionDispatcher.IsCurrentExecution);
        });
        admission.SetResult();
        Assert.True(await turn.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.False(table.TryCancel(operation));
        await table.CompletionDrained;
    }

    [Fact]
    public void ImmediateSendCompletesBeforeTheSubmitterReturnsItsCompletion()
    {
        var table = new ZLinkMeshCompletionTable();
        // A pool turn could finish before the next statement once, but not on
        // every iteration; the observing call itself must complete each send.
        for (var index = 1UL; index <= 1_000; index++)
        {
            var operation = new MeshOperationId(95, index);
            var pending = table.RegisterSend(operation);
            pending.Observe(Task.CompletedTask);
            // The entry is taken and its accounting released in the observing
            // call, so the submitter's continuation needs no dispatcher turn.
            Assert.True(pending.Completion.IsCompletedSuccessfully);
            Assert.False(table.TryCancel(operation));
            Assert.True(table.CompletionDrained.IsCompletedSuccessfully);
        }
    }

    [Fact]
    public async Task SendShutdownWinsOverLateBindingAdmission()
    {
        var table = new ZLinkMeshCompletionTable();
        var admission = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var pending = table.RegisterSend(new MeshOperationId(93, 1));
        pending.Observe(admission.Task);
        var completion = pending.Completion.AsTask();
        table.FailAll(RequestResult.Terminated);
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => completion);
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
        admission.SetResult();
        await table.CompletionDrained;
        Assert.True(completion.IsFaulted);
    }

    [Fact]
    public async Task ImmediateSendPreservesTheBindingFailure()
    {
        var table = new ZLinkMeshCompletionTable();
        var failure = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        var pending = table.RegisterSend(new MeshOperationId(94, 1));
        pending.Observe(Task.FromException(failure));
        var observed = await Assert.ThrowsAsync<ZlinkSubmitException>(() =>
            pending.Completion.AsTask()
        );
        Assert.Same(failure, observed);
        await table.CompletionDrained;
    }

    [Fact]
    public async Task DispatcherCreatedOnOwnerLane_DoesNotInheritLaneCurrentScope()
    {
        var lane = new ZLinkStateLane();
        var table = await lane.RunAsync(() => new ZLinkMeshCompletionTable());
        var operationId = new MeshOperationId(91, 1);
        var observed = new TaskCompletionSource<ZLinkStateLane?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        table.Register(operationId, (_, _) => observed.TrySetResult(ZLinkStateLane.Current));
        await lane.RunAsync(() =>
            table.Complete(MeshReceiveRecord.CompletionFailure(operationId, RequestResult.Ok), [])
        );
        Assert.Null(await observed.Task.WaitAsync(TimeSpan.FromSeconds(3)));
        await table.CompletionDrained;
    }

    [Fact]
    public void PendingOperationsHaveNoCompletionCapacityLimit()
    {
        var table = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(1, 1);
        var rejected = new MeshOperationId(1, 2);

        Assert.True(table.Register(first, static (_, _) => { }));
        Assert.True(table.Register(rejected, static (_, _) => { }));

        Assert.True(table.TryCancel(first));
        Assert.True(table.TryCancel(rejected));
    }

    [Fact]
    public void FullOperationIdentityDoesNotAliasOnEitherHalf()
    {
        var table = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(1, 7);
        var differentHigh = new MeshOperationId(2, 7);
        var differentLow = new MeshOperationId(1, 8);

        Assert.True(table.Register(first, static (_, _) => { }));
        Assert.True(table.Register(differentHigh, static (_, _) => { }));
        Assert.True(table.Register(differentLow, static (_, _) => { }));

        Assert.True(table.TryCancel(first));
        Assert.True(table.TryCancel(differentHigh));
        Assert.True(table.TryCancel(differentLow));
    }

    [Fact]
    public async Task ReplyAndCancellationHaveExactlyOneTerminalWinner()
    {
        for (var index = 1; index <= 128; index++)
        {
            var table = new ZLinkMeshCompletionTable();
            var operation = new MeshOperationId(7, checked((ulong)index));
            var winners = 0;
            Assert.True(table.Register(operation, (_, _) => Interlocked.Increment(ref winners)));
            using var start = new ManualResetEventSlim();

            var reply = Task.Run(() =>
            {
                start.Wait();
                table.Complete(
                    MeshReceiveRecord.CompletionFailure(operation, RequestResult.Terminated),
                    Array.Empty<Message>()
                );
            });
            var cancellation = Task.Run(() =>
            {
                start.Wait();
                if (table.TryCancel(operation))
                    Interlocked.Increment(ref winners);
            });

            start.Set();
            await Task.WhenAll(reply, cancellation);
            await table.CompletionDrained;
            Assert.Equal(1, winners);
        }
    }

    [Fact]
    public async Task ShutdownAndCancellationHaveExactlyOneTerminalWinner()
    {
        for (var index = 1; index <= 128; index++)
        {
            var table = new ZLinkMeshCompletionTable();
            var operation = new MeshOperationId(9, checked((ulong)index));
            var winners = 0;
            Assert.True(table.Register(operation, (_, _) => Interlocked.Increment(ref winners)));
            using var start = new ManualResetEventSlim();

            var shutdown = Task.Run(() =>
            {
                start.Wait();
                table.FailAll(RequestResult.Terminated);
            });
            var cancellation = Task.Run(() =>
            {
                start.Wait();
                if (table.TryCancel(operation))
                    Interlocked.Increment(ref winners);
            });

            start.Set();
            await Task.WhenAll(shutdown, cancellation);
            await table.CompletionDrained;
            Assert.Equal(1, winners);
        }
    }

    [Fact]
    public async Task CancellationTakesEntryBeforeDispatchingOnTheSharedLane()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(10, 1);
        var reentrant = new MeshOperationId(10, 2);
        var registered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(table.Register(operation, static (_, _) => { }));
        using var cancellation = new CancellationTokenSource();
        using var registration = table.RegisterCancellation(
            operation,
            cancellation.Token,
            () =>
            {
                Assert.True(ZLinkCompletionDispatcher.IsCurrentExecution);
                Assert.True(table.Register(reentrant, static (_, _) => { }));
                registered.TrySetResult();
            }
        );

        cancellation.Cancel();

        await registered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(table.TryCancel(operation));
        Assert.True(table.TryCancel(reentrant));
        await table.CompletionDrained;
    }

    [Fact]
    public async Task ShutdownDispatchesEveryTakenCallbackWhenOneThrows()
    {
        var table = new ZLinkMeshCompletionTable();
        var throwing = new MeshOperationId(12, 1);
        var completed = new MeshOperationId(12, 2);
        var completions = 0;
        Assert.True(
            table.Register(
                throwing,
                static (_, _) => throw new InvalidOperationException("callback")
            )
        );
        Assert.True(table.Register(completed, (_, _) => completions++));

        table.FailAll(RequestResult.Terminated);
        await table.CompletionDrained;

        Assert.Equal(1, completions);
        var closed = Assert.Throws<ZLinkFrameworkException>(() =>
            table.Register(throwing, static (_, _) => { })
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, closed.Kind);
    }

    [Fact]
    public async Task ShutdownAcrossTablesDrainsTheirPendingCallbacks()
    {
        var firstTable = new ZLinkMeshCompletionTable();
        var secondTable = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(12, 3);
        var second = new MeshOperationId(12, 4);
        Assert.True(firstTable.Register(first, static (_, _) => { }));
        Assert.True(secondTable.Register(second, static (_, _) => { }));

        firstTable.FailAll(RequestResult.Terminated);
        secondTable.FailAll(RequestResult.Terminated);
        await Task.WhenAll(firstTable.CompletionDrained, secondTable.CompletionDrained);

        var nextTable = new ZLinkMeshCompletionTable();
        var next = new MeshOperationId(12, 5);
        Assert.True(nextTable.Register(next, static (_, _) => { }));
        Assert.True(nextTable.TryCancel(next));
    }

    [Fact]
    public async Task ShutdownWaitsForBlockedCallbackWhileOtherCallbacksFinish()
    {
        var table = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(12, 6);
        var second = new MeshOperationId(12, 7);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var otherFinished = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var release = new ManualResetEventSlim();
        table.Register(
            first,
            (_, _) =>
            {
                entered.SetResult();
                release.Wait();
            }
        );
        table.Register(
            second,
            (_, _) =>
            {
                otherFinished.SetResult();
                throw new InvalidOperationException("shutdown callback");
            }
        );
        table.Complete(MeshReceiveRecord.CompletionFailure(first, RequestResult.Ok), []);
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            table.FailAll(RequestResult.Terminated);
            await otherFinished.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(table.CompletionDrained.IsCompleted);
        }
        finally
        {
            release.Set();
            await table.CompletionDrained.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Fact]
    public async Task ThrowingCallbackCannotRetainOrCorruptTheEntry()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(13, 1);
        Assert.True(
            table.Register(
                operation,
                static (_, _) => throw new InvalidOperationException("callback")
            )
        );

        table.Complete(
            MeshReceiveRecord.CompletionFailure(operation, RequestResult.TimedOut),
            Array.Empty<Message>()
        );
        await table.CompletionDrained;

        Assert.True(table.Register(operation, static (_, _) => { }));
        Assert.True(table.TryCancel(operation));
    }

    [Fact]
    public async Task ThrowingHandlerKeepsSoleOwnershipOfReplyParts()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(13, 2);
        var parts = new OwnershipProbeParts();
        Assert.True(
            table.Register(
                operation,
                (_, received) =>
                {
                    Assert.Same(parts, received);
                    throw new InvalidOperationException("callback");
                }
            )
        );

        table.Complete(
            MeshReceiveRecord.CompletionFailure(operation, RequestResult.TimedOut),
            parts
        );
        await table.CompletionDrained;

        Assert.Equal(0, parts.EnumerationCount);
    }

    [Fact]
    public async Task CompletionDispatchUsesANewTurnOutsideTheRegistryGate()
    {
        var table = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(11, 1);
        var reentrant = new MeshOperationId(11, 2);
        var callbackEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var releaseCallback = new ManualResetEventSlim();
        Assert.True(
            table.Register(
                first,
                (_, _) =>
                {
                    Assert.True(ZLinkCompletionDispatcher.IsCurrentExecution);
                    Assert.True(table.Register(reentrant, static (_, _) => { }));
                    callbackEntered.TrySetResult();
                    releaseCallback.Wait();
                }
            )
        );

        table.Complete(
            MeshReceiveRecord.CompletionFailure(first, RequestResult.Terminated),
            Array.Empty<Message>()
        );

        await callbackEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(table.TryCancel(first));
        Assert.True(table.TryCancel(reentrant));
        releaseCallback.Set();
        await table.CompletionDrained;
    }

    [Fact]
    public async Task BlockedCallbackDoesNotBlockOtherTables()
    {
        var firstTable = new ZLinkMeshCompletionTable();
        var secondTable = new ZLinkMeshCompletionTable();
        var firstOperation = new MeshOperationId(14, 1);
        var secondOperation = new MeshOperationId(14, 2);
        var firstEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var releaseFirst = new ManualResetEventSlim();
        firstTable.Register(
            firstOperation,
            (_, _) =>
            {
                firstEntered.SetResult();
                releaseFirst.Wait();
            }
        );
        secondTable.Register(
            secondOperation,
            (_, _) =>
                secondEntered.SetResult(
                    ZLinkCompletionDispatcher.IsCurrentExecution
                        && Thread.CurrentThread.IsThreadPoolThread
                )
        );
        firstTable.Complete(
            MeshReceiveRecord.CompletionFailure(firstOperation, RequestResult.Terminated),
            []
        );
        await firstEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            secondTable.Complete(
                MeshReceiveRecord.CompletionFailure(secondOperation, RequestResult.Terminated),
                []
            );
            Assert.True(await secondEntered.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.False(firstTable.CompletionDrained.IsCompleted);
            await secondTable.CompletionDrained;
        }
        finally
        {
            releaseFirst.Set();
            await firstTable.CompletionDrained;
        }
    }

    [Fact]
    public async Task DispatcherBacklogStillRegistersLaterOperations()
    {
        var table = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(15, 1);
        var second = new MeshOperationId(15, 2);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        Assert.True(
            table.Register(
                first,
                (_, _) =>
                {
                    entered.TrySetResult();
                    release.Wait();
                }
            )
        );

        table.Complete(
            MeshReceiveRecord.CompletionFailure(first, RequestResult.Terminated),
            Array.Empty<Message>()
        );
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(table.Register(second, static (_, _) => { }));
        Assert.True(table.TryCancel(second));
        release.Set();
        await table.CompletionDrained;
    }

    [Fact]
    public void CompletionTablesRegisterMoreThanTheFormerProcessWideLimit()
    {
        const int requestCount = 4_097;
        var firstTable = new ZLinkMeshCompletionTable();
        var secondTable = new ZLinkMeshCompletionTable();

        for (var index = 1; index <= requestCount; index++)
        {
            Assert.True(
                firstTable.Register(
                    new MeshOperationId(19, checked((ulong)index)),
                    static (_, _) => { }
                )
            );
            Assert.True(
                secondTable.Register(
                    new MeshOperationId(20, checked((ulong)index)),
                    static (_, _) => { }
                )
            );
        }

        for (var index = 1; index <= requestCount; index++)
        {
            Assert.True(firstTable.TryCancel(new MeshOperationId(19, checked((ulong)index))));
            Assert.True(secondTable.TryCancel(new MeshOperationId(20, checked((ulong)index))));
        }
    }

    [Fact]
    public async Task RunningCallbackDoesNotBlockAnotherTableRegistration()
    {
        var firstTable = new ZLinkMeshCompletionTable();
        var secondTable = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(21, 1);
        var second = new MeshOperationId(21, 2);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        Assert.True(
            firstTable.Register(
                first,
                (_, _) =>
                {
                    entered.TrySetResult();
                    release.Wait();
                }
            )
        );
        firstTable.Complete(
            MeshReceiveRecord.CompletionFailure(first, RequestResult.Terminated),
            Array.Empty<Message>()
        );
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(secondTable.Register(second, static (_, _) => { }));

        release.Set();
        await firstTable.CompletionDrained;
        Assert.True(secondTable.TryCancel(second));
    }

    [Fact]
    public async Task AcceptedTerminalEnqueuePathsDoNotAllocateDispatchNodes()
    {
        await WarmTerminalPathsAsync();

        var replyTable = new ZLinkMeshCompletionTable();
        var replyOperation = new MeshOperationId(16, 2);
        Assert.True(replyTable.Register(replyOperation, static (_, _) => { }));
        var replyRecord = MeshReceiveRecord.CompletionFailure(
            replyOperation,
            RequestResult.Terminated
        );
        var emptyParts = Array.Empty<Message>();
        var beforeReply = GC.GetAllocatedBytesForCurrentThread();
        replyTable.Complete(replyRecord, emptyParts);
        var replyAllocation = GC.GetAllocatedBytesForCurrentThread() - beforeReply;

        var cancellationTable = new ZLinkMeshCompletionTable();
        var cancellationOperation = new MeshOperationId(16, 3);
        Assert.True(cancellationTable.Register(cancellationOperation, static (_, _) => { }));
        using var cancellation = new CancellationTokenSource();
        using var registration = cancellationTable.RegisterCancellation(
            cancellationOperation,
            cancellation.Token,
            static () => { }
        );
        var beforeCancellation = GC.GetAllocatedBytesForCurrentThread();
        cancellation.Cancel();
        var cancellationAllocation = GC.GetAllocatedBytesForCurrentThread() - beforeCancellation;

        var closeTable = new ZLinkMeshCompletionTable();
        var closeOperation = new MeshOperationId(16, 4);
        Assert.True(closeTable.Register(closeOperation, static (_, _) => { }));
        closeTable.FailAll(RequestResult.Terminated);

        await Task.WhenAll(
            replyTable.CompletionDrained,
            cancellationTable.CompletionDrained,
            closeTable.CompletionDrained
        );

        Assert.Equal(0, replyAllocation);
        Assert.Equal(0, cancellationAllocation);
    }

    [Fact]
    public async Task ReentrantCompletionDoesNotWaitForItsParentCallback()
    {
        var table = new ZLinkMeshCompletionTable();
        var first = new MeshOperationId(17, 2);
        var second = new MeshOperationId(17, 3);
        var reentrant = new MeshOperationId(17, 4);
        var child = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var parent = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var completions = new System.Collections.Concurrent.ConcurrentQueue<MeshOperationId>();
        Assert.True(
            table.Register(
                first,
                (_, _) =>
                {
                    completions.Enqueue(first);
                    Assert.True(
                        table.Register(
                            reentrant,
                            (_, _) =>
                            {
                                completions.Enqueue(reentrant);
                                child.SetResult();
                            }
                        )
                    );
                    table.Complete(
                        MeshReceiveRecord.CompletionFailure(reentrant, RequestResult.Terminated),
                        []
                    );
                    parent.SetResult(child.Task.Wait(TimeSpan.FromSeconds(5)));
                }
            )
        );
        Assert.True(table.Register(second, (_, _) => completions.Enqueue(second)));
        table.Complete(MeshReceiveRecord.CompletionFailure(first, RequestResult.Terminated), []);
        table.Complete(MeshReceiveRecord.CompletionFailure(second, RequestResult.Terminated), []);
        Assert.True(await parent.Task.WaitAsync(TimeSpan.FromSeconds(10)));
        await table.CompletionDrained;
        Assert.Equal(3, completions.Count);
        Assert.Contains(first, completions);
        Assert.Contains(second, completions);
        Assert.Contains(reentrant, completions);
    }

    private static async Task WarmTerminalPathsAsync()
    {
        var replyTable = new ZLinkMeshCompletionTable();
        var replyOperation = new MeshOperationId(18, 1);
        Assert.True(replyTable.Register(replyOperation, static (_, _) => { }));
        replyTable.Complete(
            MeshReceiveRecord.CompletionFailure(replyOperation, RequestResult.Terminated),
            Array.Empty<Message>()
        );
        await replyTable.CompletionDrained;

        var cancellationTable = new ZLinkMeshCompletionTable();
        var cancellationOperation = new MeshOperationId(18, 2);
        Assert.True(cancellationTable.Register(cancellationOperation, static (_, _) => { }));
        using var cancellation = new CancellationTokenSource();
        using var registration = cancellationTable.RegisterCancellation(
            cancellationOperation,
            cancellation.Token,
            static () => { }
        );
        cancellation.Cancel();
        await cancellationTable.CompletionDrained;

        var closeTable = new ZLinkMeshCompletionTable();
        var closeOperation = new MeshOperationId(18, 3);
        Assert.True(closeTable.Register(closeOperation, static (_, _) => { }));
        closeTable.FailAll(RequestResult.Terminated);
        await closeTable.CompletionDrained;
    }

    private sealed class OwnershipProbeParts : IReadOnlyList<Message>
    {
        internal int EnumerationCount { get; private set; }

        public int Count => 0;

        public Message this[int index] => throw new ArgumentOutOfRangeException(nameof(index));

        public IEnumerator<Message> GetEnumerator()
        {
            EnumerationCount++;
            return Enumerable.Empty<Message>().GetEnumerator();
        }

        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
            GetEnumerator();
    }
}
