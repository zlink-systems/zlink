using Zlink.Framework.Runtime.Channels;

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
    public async Task Dispose_returns_when_host_deadline_expires_while_handler_ignores_cancellation()
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var deadline = new CancellationTokenSource();
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
            _ => { }
        );

        await queue.PostAsync(1, CancellationToken.None);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var dispose = queue.DisposeAsync().AsTask();
        await Task.Delay(100);
        Assert.False(dispose.IsCompleted);
        deadline.Cancel();
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        release.SetResult();
    }
}
