using System.Reflection;

using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class LogicalMulticastSubmitTests
{
    [Fact]
    public async Task Publish_UsesOneNativeOperationPerPublicSubmit()
    {
        var spot = DispatchProxy.Create<IZLinkBackendSpot, PublishSpotProxy>();
        var proxy = (PublishSpotProxy)(object)spot;

        await using var transport = new ZLinkSpotOutboundTransport(
            spot,
            TimeSpan.FromMilliseconds(250),
            CancellationToken.None);
        await using var pool = CreatePool();
        using var message = Message.From("publish");
        var result = await SubmitAsync(
            pool,
            () => transport.PublishCurrent(
                "events-channel",
                "events",
                [message]),
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromSeconds(1));

        Assert.Equal(SubmitResult.Ok, result);
        Assert.True(SpinWait.SpinUntil(
            () => proxy.PublishCount == 1,
            TimeSpan.FromSeconds(5)));
        Assert.Equal(1, proxy.PublishCount);
        Assert.Equal([SendFlags.None], proxy.Flags);
    }

    [Fact]
    public async Task Publish_WaitsForWorkerReservationWithinAdmissionTimeout()
    {
        await using var pool = CreatePool();
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(ZLinkWorkerSubmitResult.Accepted, pool.TrySubmitDirect(_ =>
        {
            started.TrySetResult();
            release.Wait();
        }));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var publishCount = 0;

        var pending = SubmitAsync(
            pool,
            () =>
            {
                publishCount++;
            },
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromSeconds(1)).AsTask();

        Assert.False(pending.IsCompleted);
        release.Set();
        var result = await pending.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(SubmitResult.Ok, result);
        Assert.True(SpinWait.SpinUntil(
            () => Volatile.Read(ref publishCount) == 1,
            TimeSpan.FromSeconds(5)));
        Assert.Equal(1, publishCount);
    }

    [Fact]
    public async Task Publish_ReturnsBackpressuredAfterWorkerAdmissionTimeout()
    {
        await using var pool = CreatePool();
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(ZLinkWorkerSubmitResult.Accepted, pool.TrySubmitDirect(_ =>
        {
            started.TrySetResult();
            release.Wait();
        }));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var publishCount = 0;

        var result = await SubmitAsync(
            pool,
            () =>
            {
                publishCount++;
            },
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromMilliseconds(50));

        Assert.Equal(SubmitResult.Backpressured, result);
        Assert.Equal(0, publishCount);
        release.Set();
    }

    [Fact]
    public async Task Publish_BoundsWorkerAdmissionWaitersAndNeverRunsOverflowClosures()
    {
        await using var pool = CreatePool();
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(ZLinkWorkerSubmitResult.Accepted, pool.TrySubmitDirect(_ =>
        {
            started.TrySetResult();
            release.Wait();
        }));
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var publishCount = 0;

        void Publish()
        {
            Interlocked.Increment(ref publishCount);
        }

        var waiter = SubmitAsync(
            pool,
            Publish,
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromSeconds(2)).AsTask();
        Assert.True(SpinWait.SpinUntil(
            () => pool.DirectAdmissionWaiterCount == 1,
            TimeSpan.FromSeconds(1)));

        var overflow = Enumerable.Range(0, 32)
            .Select(_ => SubmitAsync(
                pool,
                Publish,
                CancellationToken.None,
                CancellationToken.None,
                TimeSpan.FromSeconds(2)).AsTask())
            .ToArray();
        var rejected = await Task.WhenAll(overflow);

        Assert.All(rejected, result => Assert.Equal(SubmitResult.Backpressured, result));
        Assert.Equal(0, publishCount);
        Assert.Equal(1, pool.DirectAdmissionWaiterCount);
        release.Set();

        Assert.Equal(
            SubmitResult.Ok,
            await waiter.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.True(SpinWait.SpinUntil(
            () => Volatile.Read(ref publishCount) == 1,
            TimeSpan.FromSeconds(5)));
        Assert.Equal(1, publishCount);
        Assert.True(SpinWait.SpinUntil(
            () => pool.DirectAdmissionWaiterCount == 0,
            TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task Publish_CancellationAfterOperationStarts_DoesNotChangeCompletedTerminal()
    {
        await using var pool = CreatePool();
        using var release = new ManualResetEventSlim(false);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var cancellation = new CancellationTokenSource();
        var pending = SubmitAsync(
            pool,
            () =>
            {
                started.TrySetResult();
                release.Wait();
            },
            cancellation.Token,
            CancellationToken.None,
            TimeSpan.FromSeconds(1)).AsTask();
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        cancellation.Cancel();
        Assert.Equal(
            SubmitResult.Ok,
            await pending.WaitAsync(TimeSpan.FromSeconds(5)));
        release.Set();
    }

    [Fact]
    public async Task Publish_FailureAfterOperationStarts_IsObservedWithoutChangingTerminal()
    {
        await using var pool = CreatePool();
        var errorSink = new RecordingErrorSink();
        var released = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var result = await ZLinkLogicalMulticastSubmitter.SubmitAsync(
            pool,
            () => throw new InvalidOperationException("target failed"),
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromSeconds(1),
            () => released.TrySetResult(),
            errorSink);

        Assert.Equal(SubmitResult.Ok, result);
        await released.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.IsType<InvalidOperationException>(errorSink.Error);
    }

    private static ValueTask<SubmitResult> SubmitAsync(
        ZLinkWorkerPool pool,
        Action publish,
        CancellationToken cancellationToken,
        CancellationToken shutdownToken,
        TimeSpan timeout)
    {
        return ZLinkLogicalMulticastSubmitter.SubmitAsync(
            pool,
            publish,
            cancellationToken,
            shutdownToken,
            timeout,
            static () => { },
            new RecordingErrorSink());
    }

    private static ZLinkWorkerPool CreatePool()
    {
        return new ZLinkWorkerPool(0, 1, TimeSpan.FromSeconds(30), 1);
    }

    private class PublishSpotProxy : DispatchProxy
    {
        internal int PublishCount { get; private set; }

        internal List<SendFlags> Flags { get; } = [];

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendSpot.Publish) => Publish(args),
                nameof(IZLinkBackendSpot.OnSendReady) => null,
                nameof(IAsyncDisposable.DisposeAsync) => ValueTask.CompletedTask,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private object? Publish(object?[]? args)
        {
            PublishCount++;
            Flags.Add((SendFlags)args![3]!);
            return null;
        }
    }

    private sealed class RecordingErrorSink : IZLinkRuntimeFailureReporter
    {
        public Exception? Error { get; private set; }

        public void ReportHandlerException(Exception exception) => Error = exception;

        public void ReportUnhandledCallbackException(Exception exception) => Error = exception;

        public void ReportRuntimeTaskException(string taskName, Exception exception) =>
            Error = exception;
    }
}
