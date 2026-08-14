using System.Diagnostics;
using System.Text;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_publisher_async_admission
{
    private const ulong RecordHwm = 65_536UL + 1_024UL;
    private static readonly byte[] LargePayload = new byte[65_536];

    [Fact]
    public async Task fresh_publish_attempts_immediately_and_try_publish_is_dontwait()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        Configure(publisher, subscriber);
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "publisher-admission-fresh");
        publisher.Bind(endpoint);
        ConnectSubscription(publisher, subscriber, endpoint, "fresh");

        using Message fresh = Message.From("fresh-payload");
        Task submitted = publisher.Publish("fresh").Message(fresh).Async();
        Assert.True(submitted.IsCompletedSuccessfully);
        await submitted;
        Assert.Equal("fresh-payload",
            CoreTestSupport.SubscribeUtf8WithTimeout(
                subscriber, out string topic, 2_000));
        Assert.Equal("fresh", topic);

        FillTarget(publisher, "fresh");
        using Message blocked = Message.From(LargePayload);
        var started = Stopwatch.StartNew();
        bool accepted = publisher.TryPublish("fresh")
            .Message(blocked)
            .Submit();
        started.Stop();
        Assert.False(accepted);
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
    }

    [Fact]
    public async Task one_ready_epoch_does_not_head_of_line_block_another_topic()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var publisher = context.CreateXPubSocket();
        using var subscriberA = context.CreateSubSocket();
        using var subscriberB = context.CreateSubSocket();
        Configure(publisher, subscriberA);
        publisher.Options.SendTimeout = null;
        subscriberB.Options.ReceiveHighWaterMark = RecordHwm;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "publisher-admission-fair");
        publisher.Bind(endpoint);
        ConnectSubscription(publisher, subscriberA, endpoint, "a");
        ConnectSubscription(publisher, subscriberB, endpoint, "b");

        FillTarget(publisher, "a");
        FillTarget(publisher, "b");

        using var cancelA = new CancellationTokenSource();
        using Message payloadA = Message.From("pending-a");
        using Message payloadB = Message.From("pending-b");
        Task pendingA = publisher.Publish("a")
            .Message(payloadA)
            .Async(cancelA.Token);
        Task pendingB = publisher.Publish("b")
            .Message(payloadB)
            .Async();
        await Task.Delay(50);
        Assert.False(pendingA.IsCompleted);
        Assert.False(pendingB.IsCompleted);

        await DrainUntilCompletedAsync(subscriberB, pendingB);
        await pendingB.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.False(pendingA.IsCompleted);

        cancelA.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await pendingA);
    }

    [Fact]
    public async Task parked_publish_has_exact_timeout_cancel_and_close_terminals()
    {
        if (!CoreTestSupport.IsNativeAvailable()) return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        try
        {
            Configure(publisher, subscriber);
            string endpoint = CoreTestSupport.NewEndpoint(
                "inproc", "publisher-admission-terminals");
            publisher.Bind(endpoint);
            ConnectSubscription(publisher, subscriber, endpoint, "terminal");
            FillTarget(publisher, "terminal");

            publisher.Options.SendTimeout = TimeSpan.FromMilliseconds(100);
            using Message timedPayload = Message.From("timed");
            Task timed = publisher.Publish("terminal")
                .Message(timedPayload)
                .Async();
            ZlinkSubmitException timedError = await Assert.ThrowsAsync<
                ZlinkSubmitException>(async () => await timed);
            Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured,
                timedError.Result);

            publisher.Options.SendTimeout = null;
            using var cancellation = new CancellationTokenSource();
            using Message canceledPayload = Message.From("canceled");
            Task canceled = publisher.Publish("terminal")
                .Message(canceledPayload)
                .Async(cancellation.Token);
            await Task.Delay(50);
            Assert.False(canceled.IsCompleted);
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
                await canceled);

            using Message closedPayload = Message.From("closed");
            Task closed = publisher.Publish("terminal")
                .Message(closedPayload)
                .Async();
            await Task.Delay(50);
            Assert.False(closed.IsCompleted);
            publisher.Dispose();
            ZlinkSubmitException closedError = await Assert.ThrowsAsync<
                ZlinkSubmitException>(async () => await closed);
            Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated,
                closedError.Result);
        }
        finally
        {
            publisher.Dispose();
        }
    }

    private static void Configure(
        IXPubSocket publisher,
        ISubSocket subscriber)
    {
        publisher.Options.NoDrop = true;
        publisher.Options.SendHighWaterMark = RecordHwm;
        subscriber.Options.ReceiveHighWaterMark = RecordHwm;
    }

    private static void ConnectSubscription(
        IXPubSocket publisher,
        ISubSocket subscriber,
        string endpoint,
        string topic)
    {
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(topic);
        byte[] subscribed = CoreTestSupport
            .ReceiveSubscriptionEventWithTimeout(
                publisher, out bool active, 2_000);
        Assert.True(active);
        Assert.Equal(topic, Encoding.UTF8.GetString(subscribed));
    }

    private static void FillTarget(IXPubSocket publisher, string topic)
    {
        for (var attempt = 0; attempt < 64; attempt++)
        {
            using Message filler = Message.From(LargePayload);
            if (!publisher.TryPublish(topic).Message(filler).Submit())
                return;
        }

        throw new Xunit.Sdk.XunitException(
            $"Publisher target '{topic}' did not reach back-pressure.");
    }

    private static async Task DrainUntilCompletedAsync(
        ISubSocket subscriber,
        Task pending)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (!pending.IsCompleted && DateTimeOffset.UtcNow < deadline)
        {
            using var received = new TopicMessage();
            if (!subscriber.Subscribe(received, RecvFlags.DontWait))
                await Task.Delay(1);
        }
    }
}
