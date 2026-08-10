using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;

namespace Zlink.Framework.UnitTests;

public sealed class MeshCompletionTableConvergenceTests
{
    [Fact]
    public void PendingOperationBoundRejectsBeforeChangingTheTable()
    {
        var table = new ZLinkMeshCompletionTable(capacity: 1);
        var first = new MeshOperationId(1, 1);
        var rejected = new MeshOperationId(1, 2);

        Assert.True(table.Register(first, static (_, _) => { }));
        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            table.Register(rejected, static (_, _) => { }));
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, error.Kind);

        Assert.True(table.TryCancel(first));
        Assert.True(table.Register(rejected, static (_, _) => { }));
        Assert.True(table.TryCancel(rejected));
    }

    [Fact]
    public void FullOperationIdentityDoesNotAliasOnEitherHalf()
    {
        var table = new ZLinkMeshCompletionTable(capacity: 3);
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
            Assert.True(table.Register(
                operation,
                (_, _) => Interlocked.Increment(ref winners)));
            using var start = new ManualResetEventSlim();

            var reply = Task.Run(() =>
            {
                start.Wait();
                table.Complete(
                    MeshReceiveRecord.CompletionFailure(
                        operation,
                        RequestResult.Terminated),
                    Array.Empty<Message>());
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
            Assert.True(table.Register(
                operation,
                (_, _) => Interlocked.Increment(ref winners)));
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
            TaskCreationOptions.RunContinuationsAsynchronously);
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
            });

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
        Assert.True(table.Register(
            throwing,
            static (_, _) => throw new InvalidOperationException("callback")));
        Assert.True(table.Register(
            completed,
            (_, _) => completions++));

        table.FailAll(RequestResult.Terminated);
        await table.CompletionDrained;

        Assert.Equal(1, completions);
        var closed = Assert.Throws<ZLinkFrameworkException>(() =>
            table.Register(throwing, static (_, _) => { }));
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, closed.Kind);
    }

    [Fact]
    public async Task ShutdownAcrossTablesReturnsProcessWideReservations()
    {
        var dispatcher = new ZLinkCompletionDispatcher(capacity: 2);
        var firstTable = new ZLinkMeshCompletionTable(
            capacity: 2,
            dispatcher: dispatcher);
        var secondTable = new ZLinkMeshCompletionTable(
            capacity: 2,
            dispatcher: dispatcher);
        var first = new MeshOperationId(12, 3);
        var second = new MeshOperationId(12, 4);
        Assert.True(firstTable.Register(first, static (_, _) => { }));
        Assert.True(secondTable.Register(second, static (_, _) => { }));

        firstTable.FailAll(RequestResult.Terminated);
        secondTable.FailAll(RequestResult.Terminated);
        await Task.WhenAll(
            firstTable.CompletionDrained,
            secondTable.CompletionDrained);

        var nextTable = new ZLinkMeshCompletionTable(
            capacity: 1,
            dispatcher: dispatcher);
        var next = new MeshOperationId(12, 5);
        Assert.True(nextTable.Register(next, static (_, _) => { }));
        Assert.True(nextTable.TryCancel(next));
    }

    [Fact]
    public async Task ThrowingCallbackCannotRetainOrCorruptTheEntry()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(13, 1);
        Assert.True(table.Register(
            operation,
            static (_, _) => throw new InvalidOperationException("callback")));

        table.Complete(
            MeshReceiveRecord.CompletionFailure(
                operation,
                RequestResult.TimedOut),
            Array.Empty<Message>());
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
        Assert.True(table.Register(
            operation,
            (_, received) =>
            {
                Assert.Same(parts, received);
                throw new InvalidOperationException("callback");
            }));

        table.Complete(
            MeshReceiveRecord.CompletionFailure(
                operation,
                RequestResult.TimedOut),
            parts);
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
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var releaseCallback = new ManualResetEventSlim();
        Assert.True(table.Register(first, (_, _) =>
        {
            Assert.True(ZLinkCompletionDispatcher.IsCurrentExecution);
            Assert.True(table.Register(reentrant, static (_, _) => { }));
            callbackEntered.TrySetResult();
            releaseCallback.Wait();
        }));

        table.Complete(
            MeshReceiveRecord.CompletionFailure(
                first,
                RequestResult.Terminated),
            Array.Empty<Message>());

        await callbackEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(table.TryCancel(first));
        Assert.True(table.TryCancel(reentrant));
        releaseCallback.Set();
        await table.CompletionDrained;
    }

    [Fact]
    public async Task TablesShareOneCompletionLaneWithoutPerCallbackThreads()
    {
        var firstTable = new ZLinkMeshCompletionTable();
        var secondTable = new ZLinkMeshCompletionTable();
        var firstOperation = new MeshOperationId(14, 1);
        var secondOperation = new MeshOperationId(14, 2);
        var firstEntered = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondEntered = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var releaseFirst = new ManualResetEventSlim();

        Assert.True(firstTable.Register(firstOperation, (_, _) =>
        {
            firstEntered.TrySetResult(Environment.CurrentManagedThreadId);
            releaseFirst.Wait();
        }));
        Assert.True(secondTable.Register(secondOperation, (_, _) =>
            secondEntered.TrySetResult(Environment.CurrentManagedThreadId)));

        firstTable.Complete(
            MeshReceiveRecord.CompletionFailure(
                firstOperation,
                RequestResult.Terminated),
            Array.Empty<Message>());
        var workerThread = await firstEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        secondTable.Complete(
            MeshReceiveRecord.CompletionFailure(
                secondOperation,
                RequestResult.Terminated),
            Array.Empty<Message>());

        Assert.False(secondEntered.Task.IsCompleted);
        releaseFirst.Set();
        var secondThread = await secondEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.WhenAll(
            firstTable.CompletionDrained,
            secondTable.CompletionDrained);
        Assert.Equal(workerThread, secondThread);
    }

    [Fact]
    public async Task DispatcherBacklogKeepsTheExistingAdmissionReservation()
    {
        var table = new ZLinkMeshCompletionTable(capacity: 1);
        var first = new MeshOperationId(15, 1);
        var second = new MeshOperationId(15, 2);
        var entered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        Assert.True(table.Register(first, (_, _) =>
        {
            entered.TrySetResult();
            release.Wait();
        }));

        table.Complete(
            MeshReceiveRecord.CompletionFailure(
                first,
                RequestResult.Terminated),
            Array.Empty<Message>());
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var full = Assert.Throws<ZLinkFrameworkException>(() =>
            table.Register(second, static (_, _) => { }));
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, full.Kind);
        release.Set();
        await table.CompletionDrained;

        Assert.True(table.Register(second, static (_, _) => { }));
        Assert.True(table.TryCancel(second));
    }

    [Fact]
    public void ProcessWideBoundAggregatesAcrossCompletionTables()
    {
        const int halfCapacity = 2_048;
        var dispatcher = new ZLinkCompletionDispatcher(capacity: 4_096);
        var firstTable = new ZLinkMeshCompletionTable(
            capacity: 4_096,
            dispatcher: dispatcher);
        var secondTable = new ZLinkMeshCompletionTable(
            capacity: 4_096,
            dispatcher: dispatcher);

        for (var index = 1; index <= halfCapacity; index++)
        {
            Assert.True(firstTable.Register(
                new MeshOperationId(19, checked((ulong)index)),
                static (_, _) => { }));
            Assert.True(secondTable.Register(
                new MeshOperationId(20, checked((ulong)index)),
                static (_, _) => { }));
        }

        var rejected = Assert.Throws<ZLinkFrameworkException>(() =>
            secondTable.Register(
                new MeshOperationId(20, halfCapacity + 1),
                static (_, _) => { }));
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, rejected.Kind);

        for (var index = 1; index <= halfCapacity; index++)
        {
            Assert.True(firstTable.TryCancel(
                new MeshOperationId(19, checked((ulong)index))));
            Assert.True(secondTable.TryCancel(
                new MeshOperationId(20, checked((ulong)index))));
        }
    }

    [Fact]
    public async Task RunningCallbackBlocksAnotherTableUntilItsSlotIsReleased()
    {
        var dispatcher = new ZLinkCompletionDispatcher(capacity: 1);
        var firstTable = new ZLinkMeshCompletionTable(
            capacity: 1,
            dispatcher: dispatcher);
        var secondTable = new ZLinkMeshCompletionTable(
            capacity: 1,
            dispatcher: dispatcher);
        var first = new MeshOperationId(21, 1);
        var second = new MeshOperationId(21, 2);
        var entered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        Assert.True(firstTable.Register(first, (_, _) =>
        {
            entered.TrySetResult();
            release.Wait();
        }));
        firstTable.Complete(
            MeshReceiveRecord.CompletionFailure(
                first,
                RequestResult.Terminated),
            Array.Empty<Message>());
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var full = Assert.Throws<ZLinkFrameworkException>(() =>
            secondTable.Register(second, static (_, _) => { }));
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, full.Kind);

        release.Set();
        await firstTable.CompletionDrained;
        Assert.True(secondTable.Register(second, static (_, _) => { }));
        Assert.True(secondTable.TryCancel(second));
    }

    [Fact]
    public async Task AcceptedTerminalEnqueuePathsDoNotAllocateDispatchNodes()
    {
        await WarmTerminalPathsAsync();

        var blocker = new ZLinkMeshCompletionTable();
        var blockerOperation = new MeshOperationId(16, 1);
        var blockerEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var releaseBlocker = new ManualResetEventSlim();
        Assert.True(blocker.Register(blockerOperation, (_, _) =>
        {
            blockerEntered.TrySetResult();
            releaseBlocker.Wait();
        }));
        blocker.Complete(
            MeshReceiveRecord.CompletionFailure(
                blockerOperation,
                RequestResult.Terminated),
            Array.Empty<Message>());
        await blockerEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var replyTable = new ZLinkMeshCompletionTable();
        var replyOperation = new MeshOperationId(16, 2);
        Assert.True(replyTable.Register(replyOperation, static (_, _) => { }));
        var replyRecord = MeshReceiveRecord.CompletionFailure(
            replyOperation,
            RequestResult.Terminated);
        var emptyParts = Array.Empty<Message>();
        var beforeReply = GC.GetAllocatedBytesForCurrentThread();
        replyTable.Complete(replyRecord, emptyParts);
        var replyAllocation = GC.GetAllocatedBytesForCurrentThread() - beforeReply;

        var cancellationTable = new ZLinkMeshCompletionTable();
        var cancellationOperation = new MeshOperationId(16, 3);
        Assert.True(cancellationTable.Register(
            cancellationOperation,
            static (_, _) => { }));
        using var cancellation = new CancellationTokenSource();
        using var registration = cancellationTable.RegisterCancellation(
            cancellationOperation,
            cancellation.Token,
            static () => { });
        var beforeCancellation = GC.GetAllocatedBytesForCurrentThread();
        cancellation.Cancel();
        var cancellationAllocation =
            GC.GetAllocatedBytesForCurrentThread() - beforeCancellation;

        var closeTable = new ZLinkMeshCompletionTable();
        var closeOperation = new MeshOperationId(16, 4);
        Assert.True(closeTable.Register(closeOperation, static (_, _) => { }));
        var beforeClose = GC.GetAllocatedBytesForCurrentThread();
        closeTable.FailAll(RequestResult.Terminated);
        var closeAllocation = GC.GetAllocatedBytesForCurrentThread() - beforeClose;

        releaseBlocker.Set();
        await Task.WhenAll(
            blocker.CompletionDrained,
            replyTable.CompletionDrained,
            cancellationTable.CompletionDrained,
            closeTable.CompletionDrained);

        Assert.Equal(0, replyAllocation);
        Assert.Equal(0, cancellationAllocation);
        Assert.Equal(0, closeAllocation);
    }

    [Fact]
    public async Task ReentrantTerminalIsQueuedAfterAlreadyAcceptedWork()
    {
        var dispatcher = new ZLinkCompletionDispatcher(capacity: 3);
        var blocker = new ZLinkMeshCompletionTable(
            capacity: 1,
            dispatcher: dispatcher);
        var blockerOperation = new MeshOperationId(17, 1);
        var blockerEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var releaseBlocker = new ManualResetEventSlim();
        Assert.True(blocker.Register(blockerOperation, (_, _) =>
        {
            blockerEntered.TrySetResult();
            releaseBlocker.Wait();
        }));
        blocker.Complete(
            MeshReceiveRecord.CompletionFailure(
                blockerOperation,
                RequestResult.Terminated),
            Array.Empty<Message>());
        await blockerEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var table = new ZLinkMeshCompletionTable(
            capacity: 3,
            dispatcher: dispatcher);
        var first = new MeshOperationId(17, 2);
        var second = new MeshOperationId(17, 3);
        var reentrant = new MeshOperationId(17, 4);
        var order = new List<int>(capacity: 3);
        Assert.True(table.Register(first, (_, _) =>
        {
            order.Add(1);
            Assert.True(table.Register(reentrant, (_, _) => order.Add(3)));
            table.Complete(
                MeshReceiveRecord.CompletionFailure(
                    reentrant,
                    RequestResult.Terminated),
                Array.Empty<Message>());
        }));
        Assert.True(table.Register(second, (_, _) => order.Add(2)));
        table.Complete(
            MeshReceiveRecord.CompletionFailure(
                first,
                RequestResult.Terminated),
            Array.Empty<Message>());
        table.Complete(
            MeshReceiveRecord.CompletionFailure(
                second,
                RequestResult.Terminated),
            Array.Empty<Message>());

        releaseBlocker.Set();
        await Task.WhenAll(
            blocker.CompletionDrained,
            table.CompletionDrained);

        Assert.Equal([1, 2, 3], order);
    }

    private static async Task WarmTerminalPathsAsync()
    {
        var replyTable = new ZLinkMeshCompletionTable();
        var replyOperation = new MeshOperationId(18, 1);
        Assert.True(replyTable.Register(replyOperation, static (_, _) => { }));
        replyTable.Complete(
            MeshReceiveRecord.CompletionFailure(
                replyOperation,
                RequestResult.Terminated),
            Array.Empty<Message>());
        await replyTable.CompletionDrained;

        var cancellationTable = new ZLinkMeshCompletionTable();
        var cancellationOperation = new MeshOperationId(18, 2);
        Assert.True(cancellationTable.Register(
            cancellationOperation,
            static (_, _) => { }));
        using var cancellation = new CancellationTokenSource();
        using var registration = cancellationTable.RegisterCancellation(
            cancellationOperation,
            cancellation.Token,
            static () => { });
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
