using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class HttpExecutionSchedulerTests
{
    [Fact]
    public async Task Captured_http_callback_is_posted_as_a_new_serial_turn()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None);
        var callbackPosted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var callbackRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseOwner = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var owner = queue.RunAsync(
            async _ =>
            {
                // 04-async-execution-policy.ko.md: HTTP callback capture는 yield가
                // 허용된 application callback turn에서만 성립한다.
                using var scope = ZLinkApplicationExecutionContext.Push(
                    new ZLinkApplicationExecutionScope(
                        "http-scheduler-spot",
                        ZLinkUserSpotExecutionMode.SpotWide,
                        ActorId: null,
                        YieldAllowed: true));
                var scheduler = new ZLinkSpotHttpExecutionScheduler();
                var turn = scheduler.Capture()
                           ?? throw new InvalidOperationException("HTTP execution turn was not captured.");
                turn.Post(() => callbackRan.TrySetResult());
                callbackPosted.TrySetResult();
                await releaseOwner.Task.ConfigureAwait(false);
            },
            CancellationToken.None).AsTask();

        await callbackPosted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(callbackRan.Task.IsCompleted);

        releaseOwner.TrySetResult();
        await owner.WaitAsync(TimeSpan.FromSeconds(5));
        await callbackRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }
}
