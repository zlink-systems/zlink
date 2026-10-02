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
}
