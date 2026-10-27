using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ChannelApplicationDispatchQueueShutdownTests
{
    [Fact]
    public async Task Dispose_waits_for_in_flight_dispatch_without_its_own_time_budget()
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var finished = 0;
        var queue = new ZLinkChannelApplicationDispatchQueue<int>(
            "shutdown-test",
            new AuditRuntimeFailureReporter(),
            CancellationToken.None,
            CancellationToken.None,
            async (_, _) =>
            {
                started.SetResult();
                await Task.Delay(TimeSpan.FromMilliseconds(1500));
                Volatile.Write(ref finished, 1);
            },
            _ => { }
        );

        await queue.PostAsync(1, CancellationToken.None);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await queue.DisposeAsync();

        Assert.Equal(1, Volatile.Read(ref finished));
    }

    [Fact]
    public async Task Queue_built_in_a_state_lane_turn_dispatches_off_that_lane()
    {
        // A fanout subscriber registers its dispatch queue inside a state-lane turn. A handler
        // dispatched by that queue must be able to call runtime surfaces that enter the same
        // lane (06-state-ownership-and-lanes §6 type 2).
        var lane = new ZLinkStateLane();
        var onLane = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var queue = await lane.RunAsync(() =>
            new ZLinkChannelApplicationDispatchQueue<int>(
                "lane-test",
                new AuditRuntimeFailureReporter(),
                CancellationToken.None,
                CancellationToken.None,
                (_, _) =>
                {
                    onLane.SetResult(lane.IsOnLane);
                    return ValueTask.CompletedTask;
                },
                _ => { }
            )
        );

        await queue.PostAsync(1, CancellationToken.None);

        Assert.False(await onLane.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        await queue.DisposeAsync();
        await lane.DisposeAsync();
    }

    [Fact]
    public async Task Reporter_failure_is_observable_and_does_not_skip_owned_cleanup()
    {
        var dispatchFailure = new InvalidOperationException("dispatch failed");
        var reportingFailure = new InvalidOperationException("reporting failed");
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var rejected = 0;
        var queue = new ZLinkChannelApplicationDispatchQueue<int>(
            "failed-reporter-test",
            new ThrowingReporter(reportingFailure),
            CancellationToken.None,
            CancellationToken.None,
            async (_, _) =>
            {
                entered.TrySetResult();
                await release.Task;
                throw dispatchFailure;
            },
            _ => Interlocked.Increment(ref rejected)
        );
        Assert.True(await queue.PostAsync(1, CancellationToken.None));
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(await queue.PostAsync(2, CancellationToken.None));
        var disposal = queue.DisposeAsync().AsTask();
        release.TrySetResult();
        var failure = await Assert.ThrowsAnyAsync<Exception>(() =>
            disposal.WaitAsync(TimeSpan.FromSeconds(5))
        );
        var aggregate = Assert.IsType<AggregateException>(failure).Flatten();
        Assert.Contains(dispatchFailure, aggregate.InnerExceptions);
        Assert.Contains(reportingFailure, aggregate.InnerExceptions);
        Assert.Equal(1, rejected);
        Assert.False(
            await queue.ReplyGate.TryInvokeAsync(() => Assert.Fail("Reply gate stayed open"))
        );
    }

    private sealed class ThrowingReporter(Exception failure) : IZLinkRuntimeFailureReporter
    {
        public void ReportHandlerException(Exception exception) => throw failure;

        public void ReportUnhandledCallbackException(Exception exception) => throw failure;

        public void ReportRuntimeTaskException(string taskName, Exception exception) =>
            throw failure;
    }

    [Fact]
    public async Task Dispose_returns_when_host_deadline_expires_while_handler_ignores_cancellation()
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var deadline = new CancellationTokenSource();
        var rejected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var queue = new ZLinkChannelApplicationDispatchQueue<int>(
            "deadline-test",
            new AuditRuntimeFailureReporter(),
            CancellationToken.None,
            deadline.Token,
            async (_, _) =>
            {
                started.SetResult();
                await release.Task;
            },
            _ => rejected.TrySetResult()
        );

        await queue.PostAsync(1, CancellationToken.None);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(await queue.PostAsync(2, CancellationToken.None));
        var dispose = queue.DisposeAsync().AsTask();
        await Task.Delay(100);
        Assert.False(dispose.IsCompleted);
        deadline.Cancel();
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        release.SetResult();
        // Late cleanup must release its process source before the next test starts.
        await rejected.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }
}
