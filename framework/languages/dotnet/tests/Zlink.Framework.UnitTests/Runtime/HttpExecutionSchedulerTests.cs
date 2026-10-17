using Zlink.Framework.AspNetCore;
using Zlink.HttpClient;

namespace Zlink.Framework.UnitTests;

public sealed class HttpExecutionSchedulerTests
{
    private static readonly TimeSpan TestTimeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task Captured_http_callback_is_posted_as_a_new_serial_turn()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
        var callbackPosted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var callbackRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseOwner = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        var owner = queue
            .RunAsync(
                async _ =>
                {
                    // 04-async-execution-policy.ko.md: HTTP callback capture는 yield가
                    // 허용된 application callback turn에서만 성립한다.
                    using var scope = ZLinkApplicationExecutionContext.Push(
                        new ZLinkApplicationExecutionScope(
                            "http-scheduler-spot",
                            ZLinkUserSpotExecutionMode.SpotWide,
                            ActorId: null,
                            YieldAllowed: true
                        )
                    );
                    var scheduler = new ZLinkSpotHttpExecutionScheduler();
                    var turn =
                        scheduler.Capture()
                        ?? throw new InvalidOperationException(
                            "HTTP execution turn was not captured."
                        );
                    turn.Post(() => callbackRan.TrySetResult());
                    callbackPosted.TrySetResult();
                    await releaseOwner.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();

        await callbackPosted.Task.WaitAsync(TestTimeout);
        Assert.False(callbackRan.Task.IsCompleted);

        releaseOwner.TrySetResult();
        await owner.WaitAsync(TestTimeout);
        await callbackRan.Task.WaitAsync(TestTimeout);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task Yield_admission_rejects_second_call_before_submission(bool ready, bool http)
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
        var result = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var attempted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var submitted = 0;
        Exception? rejection = null;
        var owner = queue
            .RunAsync(
                async token =>
                {
                    using var scope = ZLinkApplicationExecutionContext.Push(
                        new ZLinkApplicationExecutionScope(
                            "yield-spot",
                            ZLinkUserSpotExecutionMode.SpotWide,
                            null,
                            true
                        )
                    );
                    ValueTask<int> Submit(CancellationToken ct)
                    {
                        Interlocked.Increment(ref submitted);
                        return ready ? ValueTask.FromResult(1) : new ValueTask<int>(result.Task);
                    }
                    var first = ZLinkApplicationExecutionContext
                        .RequireYieldTurn("first")
                        .YieldFrameworkCallAsync(Submit, CancellationToken.None)
                        .AsTask();
                    var captured = new ZLinkSpotHttpExecutionScheduler().Capture()!;
                    async Task<int> SecondAsync()
                    {
                        try
                        {
                            return http
                                ? await captured.YieldAsync(Submit)
                                : await ZLinkApplicationExecutionContext
                                    .RequireYieldTurn("second")
                                    .YieldFrameworkCallAsync(Submit, CancellationToken.None);
                        }
                        finally
                        {
                            attempted.TrySetResult();
                        }
                    }
                    try
                    {
                        await Task.WhenAll(first, SecondAsync()).WaitAsync(TestTimeout);
                    }
                    catch (ZLinkFrameworkException error)
                    {
                        rejection = error;
                    }
                },
                CancellationToken.None
            )
            .AsTask();
        await attempted.Task.WaitAsync(TestTimeout);
        result.TrySetResult(1);
        await owner.WaitAsync(TestTimeout);
        Assert.Equal(1, submitted);
        Assert.Equal(
            ZLinkFrameworkErrorKind.InvalidOperation,
            Assert.IsType<ZLinkFrameworkException>(rejection).Kind
        );
        await queue.RunAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
    }

    [Fact]
    public async Task Captured_http_yield_rejects_completed_owner_before_submission()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
        IZLinkHttpExecutionTurn? captured = null;
        await queue.RunAsync(
            _ =>
            {
                using var scope = ZLinkApplicationExecutionContext.Push(
                    new ZLinkApplicationExecutionScope(
                        "http-spot",
                        ZLinkUserSpotExecutionMode.SpotWide,
                        null,
                        true
                    )
                );
                captured = new ZLinkSpotHttpExecutionScheduler().Capture();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None
        );
        var submitted = 0;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await captured!.YieldAsync<int>(_ =>
            {
                submitted++;
                return ValueTask.FromResult(1);
            })
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.Equal(0, submitted);
    }

    [Fact]
    public async Task Yield_admission_is_renewed_after_each_resume()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
        var first = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var second = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var resumed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var owner = queue
            .RunAsync(
                async _ =>
                {
                    using var scope = ZLinkApplicationExecutionContext.Push(
                        new ZLinkApplicationExecutionScope(
                            "yield-spot",
                            ZLinkUserSpotExecutionMode.SpotWide,
                            null,
                            true
                        )
                    );
                    var captured = new ZLinkSpotHttpExecutionScheduler().Capture()!;
                    var pending = captured.YieldAsync<int>(_ => new ValueTask<int>(first.Task));
                    started.TrySetResult();
                    Assert.Equal(1, await pending);
                    pending = captured.YieldAsync<int>(_ => new ValueTask<int>(second.Task));
                    resumed.TrySetResult();
                    Assert.Equal(2, await pending);
                },
                CancellationToken.None
            )
            .AsTask();
        await started.Task.WaitAsync(TestTimeout);
        first.TrySetResult(1);
        await resumed.Task.WaitAsync(TestTimeout);
        second.TrySetResult(2);
        await owner.WaitAsync(TestTimeout);
        await queue.RunAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
    }

    [Fact]
    public async Task Completed_handler_tail_cannot_submit_Yield()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? tail = null;
        var submitted = 0;
        await queue.RunAsync(
            _ =>
            {
                using var scope = ZLinkApplicationExecutionContext.Push(
                    new ZLinkApplicationExecutionScope(
                        "tail-spot",
                        ZLinkUserSpotExecutionMode.SpotWide,
                        null,
                        true
                    )
                );
                tail = Task.Run(async () =>
                {
                    await release.Task;
                    var error = Assert.Throws<ZLinkFrameworkException>(() =>
                        ZLinkApplicationExecutionContext
                            .RequireYieldTurn("tail")
                            .YieldFrameworkCallAsync<int>(
                                _ =>
                                {
                                    submitted++;
                                    return ValueTask.FromResult(1);
                                },
                                CancellationToken.None
                            )
                    );
                    Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
                });
                return ValueTask.CompletedTask;
            },
            CancellationToken.None
        );
        release.TrySetResult();
        await tail!.WaitAsync(TestTimeout);
        Assert.Equal(0, submitted);
    }
}
