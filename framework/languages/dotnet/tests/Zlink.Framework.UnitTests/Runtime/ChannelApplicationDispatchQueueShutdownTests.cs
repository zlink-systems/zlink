using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ChannelApplicationDispatchQueueShutdownTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Retained_dispatches_finish_and_report_before_disposal(bool reporterThrows)
    {
        var started = new TaskCompletionSource[2];
        var release = new TaskCompletionSource[2];
        for (var i = 0; i < 2; i++)
        {
            started[i] = new(TaskCreationOptions.RunContinuationsAsynchronously);
            release[i] = new(TaskCreationOptions.RunContinuationsAsynchronously);
        }
        var failure = new InvalidOperationException("retained dispatch failed");
        var reportingFailure = new InvalidOperationException("retained reporting failed");
        var audit = new AuditRuntimeFailureReporter();
        var finished = 0;
        var queue = new ZLinkChannelApplicationDispatchQueue<int>(
            "retained-shutdown",
            reporterThrows ? new ThrowingReporter(reportingFailure) : audit,
            CancellationToken.None,
            CancellationToken.None,
            (index, _) => DispatchAsync(index),
            _ => Assert.Fail("Accepted record was rejected")
        );
        Task? disposal = null;
        try
        {
            Assert.True(await queue.PostAsync(0, CancellationToken.None));
            await started[0].Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(await queue.PostAsync(1, CancellationToken.None));
            await started[1].Task.WaitAsync(TimeSpan.FromSeconds(5));
            disposal = queue.DisposeAsync().AsTask();
            Assert.False(disposal.IsCompleted);
            release[1].TrySetResult();
            await queue.ReplyGate.TryInvokeAsync(() => { });
            Assert.False(disposal.IsCompleted);
        }
        finally
        {
            release[0].TrySetResult();
            release[1].TrySetResult();
            disposal ??= queue.DisposeAsync().AsTask();
            if (reporterThrows)
            {
                var error = await Assert.ThrowsAnyAsync<Exception>(() =>
                    disposal.WaitAsync(TimeSpan.FromSeconds(5))
                );
                var errors = Assert.IsType<AggregateException>(error).Flatten().InnerExceptions;
                Assert.Contains(failure, errors);
                Assert.Contains(reportingFailure, errors);
            }
            else
                await disposal.WaitAsync(TimeSpan.FromSeconds(5));
        }
        Assert.Equal(2, Volatile.Read(ref finished));
        if (!reporterThrows)
            Assert.Same(failure, Assert.Single(audit.Failures));
        Assert.False(
            await queue.ReplyGate.TryInvokeAsync(() => Assert.Fail("Reply gate stayed open"))
        );

        async ValueTask DispatchAsync(int index)
        {
            started[index].TrySetResult();
            try
            {
                await release[index].Task;
                if (index == 0)
                    throw failure;
            }
            finally
            {
                Interlocked.Increment(ref finished);
            }
        }
    }

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
        var disposal = queue.DisposeAsync().AsTask();
        Assert.False(await queue.PostAsync(2, CancellationToken.None));
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
        var terminal = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
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
                terminal.TrySetResult();
            },
            _ => rejected.TrySetResult()
        );

        await queue.PostAsync(1, CancellationToken.None);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var dispose = queue.DisposeAsync().AsTask();
        Assert.False(await queue.PostAsync(2, CancellationToken.None));
        await Task.Delay(100);
        Assert.False(dispose.IsCompleted);
        deadline.Cancel();
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        release.SetResult();
        await terminal.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await rejected.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }
}
