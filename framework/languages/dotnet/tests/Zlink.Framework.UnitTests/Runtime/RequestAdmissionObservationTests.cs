using System.Diagnostics;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestAdmissionObservationTests
{
    private static async ValueTask<int> AcceptedReply(
        Task<int> reply,
        ZLinkRequestAdmission admission
    )
    {
        admission.Complete();
        return await reply.ConfigureAwait(false);
    }

    private static ValueTask<int> Submit(Task<int> reply) =>
        ZLinkRequestAdmission
            .Run(reply, static (task, admission) => AcceptedReply(task, admission))
            .Reply;

    [Fact]
    public async Task Accepted_pending_reply_allocates_only_admission_and_execution_storage()
    {
        const int count = 512;
        var baseline = new ValueTask<int>[count];
        var actual = new ValueTask<int>[count];
        var baselineSources = Enumerable
            .Range(0, count)
            .Select(_ => new TaskCompletionSource<int>())
            .ToArray();
        var actualSources = Enumerable
            .Range(0, count)
            .Select(_ => new TaskCompletionSource<int>())
            .ToArray();
        var source = new TaskCompletionSource<int>();
        // Both loops execute the same compiler-async operation and ordinary
        // admission ctor. Retain all replies pending until after measurement.
        var warmBaseline = AcceptedReply(source.Task, new ZLinkRequestAdmission());
        var warmActual = Submit(source.Task);
        var started = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < count; index++)
            baseline[index] = AcceptedReply(
                baselineSources[index].Task,
                new ZLinkRequestAdmission()
            );
        var expected = GC.GetAllocatedBytesForCurrentThread() - started;
        started = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < count; index++)
            actual[index] = Submit(actualSources[index].Task);
        var measured = GC.GetAllocatedBytesForCurrentThread() - started;
        var pending = actual.All(reply => !reply.IsCompleted);
        source.SetResult(7);
        foreach (var pendingSource in baselineSources)
            pendingSource.SetResult(7);
        foreach (var pendingSource in actualSources)
            pendingSource.SetResult(7);
        Assert.Equal(7, await warmBaseline);
        Assert.Equal(7, await warmActual);
        foreach (var reply in baseline)
            Assert.Equal(7, await reply);
        foreach (var reply in actual)
            Assert.Equal(7, await reply);
        Assert.True(pending);
        Assert.Equal(expected, measured);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Accepted_reply_preserves_failure_identity_and_cancellation(bool cancel)
    {
        using var cancellation = new CancellationTokenSource();
        var source = new TaskCompletionSource<int>();
        var request = ZLinkRequestAdmission.Run(
            source.Task,
            static (task, admission) => AcceptedReply(task, admission)
        );
        Assert.True(request.Admitted.IsCompletedSuccessfully);
        Assert.False(request.Reply.IsCompleted);
        var expected = new InvalidOperationException("reply failed");
        if (cancel)
        {
            cancellation.Cancel();
            source.SetCanceled(cancellation.Token);
            var reply = request.AsTask();
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => reply);
            Assert.Equal(cancellation.Token, error.CancellationToken);
            Assert.True(reply.IsCanceled);
        }
        else
        {
            source.SetException(expected);
            var reply = request.AsTask();
            Assert.Same(expected, await Assert.ThrowsAsync<InvalidOperationException>(() => reply));
            Assert.True(reply.IsFaulted);
        }
        await request.Admitted;
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Accepted_async_execution_normalizes_faulted_cancellation(bool failedBeforeRun)
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var expected = new OperationCanceledException(cancellation.Token);
        var source = new TaskCompletionSource<int>();
        if (failedBeforeRun)
            source.SetException(expected);
        var request = ZLinkRequestAdmission.Run(
            source.Task,
            static (task, admission) => AcceptedReply(task, admission)
        );
        Assert.True(request.Admitted.IsCompletedSuccessfully);
        if (!failedBeforeRun)
            source.SetException(expected);
        Assert.True(source.Task.IsFaulted);
        var reply = request.AsTask();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => reply);
        Assert.Same(expected, error);
        Assert.Equal(cancellation.Token, error.CancellationToken);
        Assert.True(reply.IsCanceled);
        await request.Admitted;
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task Synchronous_execute_failure_remains_asynchronous(
        bool cancel,
        bool admittedBeforeThrow
    )
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        Exception expected = cancel
            ? new OperationCanceledException(cancellation.Token)
            : new InvalidOperationException("execute failed");
        var request = ZLinkRequestAdmission.Run<int>(admission =>
        {
            if (admittedBeforeThrow)
                admission.Complete();
            throw expected;
        });
        var reply = request.AsTask();
        var admission = request.Admitted.AsTask();
        var replyError = await Assert.ThrowsAnyAsync<Exception>(() => reply);
        Assert.Same(expected, replyError);
        if (admittedBeforeThrow)
            await admission;
        else
            Assert.Same(expected, await Assert.ThrowsAnyAsync<Exception>(() => admission));
        Assert.Equal(cancel, reply.IsCanceled);
        Assert.Equal(admittedBeforeThrow, admission.IsCompletedSuccessfully);
        Assert.Equal(!admittedBeforeThrow, admission.IsFaulted);
        if (cancel)
            Assert.Equal(
                cancellation.Token,
                ((OperationCanceledException)replyError).CancellationToken
            );
    }

    // Public snapshot entry; setup, reply completion and join are outside the
    // producer timing/allocation region. Whole-fixture CPU is reported separately.
    public static async Task<double[]> MeasureAcceptedAdmission(int count)
    {
        var replies = new ValueTask<int>[count];
        var source = new TaskCompletionSource<int>();
        using var process = Process.GetCurrentProcess();
        var cpu = process.TotalProcessorTime.Ticks;
        var gen0 = GC.CollectionCount(0);
        var gen1 = GC.CollectionCount(1);
        var gen2 = GC.CollectionCount(2);
        var bytes = GC.GetAllocatedBytesForCurrentThread();
        var started = Stopwatch.GetTimestamp();
        for (var index = 0; index < count; index++)
            replies[index] = Submit(source.Task);
        var elapsed = Stopwatch.GetTimestamp() - started;
        var allocated = GC.GetAllocatedBytesForCurrentThread() - bytes;
        source.SetResult(7);
        foreach (var reply in replies)
            if (await reply != 7)
                throw new InvalidOperationException("Incorrect reply");
        return
        [
            elapsed * 1_000_000_000.0 / Stopwatch.Frequency / count,
            (double)allocated / count,
            (process.TotalProcessorTime.Ticks - cpu) / (double)TimeSpan.TicksPerMillisecond,
            GC.CollectionCount(0) - gen0,
            GC.CollectionCount(1) - gen1,
            GC.CollectionCount(2) - gen2,
        ];
    }
}
