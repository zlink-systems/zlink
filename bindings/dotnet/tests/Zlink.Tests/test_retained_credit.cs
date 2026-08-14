using System.Text;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_retained_credit
{
    [Fact]
    public void ordinary_received_returns_credit_at_dequeue()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc",
            "ordinary-received-credit");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        using Message first = Message.From("first");
        using Message second = Message.From("second");
        Assert.True(sender.Send().Message(first).Message(second).Submit());

        using var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                receiver.Recv(received, RecvFlags.DontWait),
            2000));
        Assert.Equal(2, received.Parts.Count);

        CoreHwmBudgetSnapshot snapshot = context.GetCoreHwmBudgetSnapshot();
        Assert.Equal(0UL, snapshot.OutstandingApplicationLeaseCount);
        Assert.Equal(0UL, snapshot.ApplicationAccountedBytes);
        Assert.Equal(snapshot.CoreQueueAccountedBytes,
            snapshot.CurrentAccountedBytes);
    }

    [Fact]
    public void retained_received_holds_credit_until_dispose()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc",
            "retained-received");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        using Message first = Message.From("first");
        using Message second = Message.From("second");
        Assert.True(sender.Send().Message(first).Message(second).Submit());

        Assert.True(CoreTestSupport.WaitUntil(() =>
                context.GetCoreHwmBudgetSnapshot().CurrentAccountedBytes > 0,
            2000));
        CoreHwmBudgetSnapshot queued =
            context.GetCoreHwmBudgetSnapshot();

        var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                receiver.RecvRetained(received, RecvFlags.DontWait),
            2000));
        Assert.Equal(2, received.Parts.Count);
        Assert.Equal("first", received.Parts[0].GetString());
        Assert.Equal("second", received.Parts[1].GetString());

        CoreHwmBudgetSnapshot held = context.GetCoreHwmBudgetSnapshot();
        Assert.Equal(2UL, held.OutstandingApplicationLeaseCount);
        Assert.True(held.ApplicationAccountedBytes > 0);
        Assert.Equal(held.CoreQueueAccountedBytes
                     + held.ApplicationAccountedBytes,
            held.CurrentAccountedBytes);
        Assert.Equal(queued.CurrentAccountedBytes,
            held.CurrentAccountedBytes);

        received.Dispose();
        Assert.True(CoreTestSupport.WaitUntil(() =>
        {
            CoreHwmBudgetSnapshot released =
                context.GetCoreHwmBudgetSnapshot();
            return released.OutstandingApplicationLeaseCount == 0
                   && released.ApplicationAccountedBytes == 0;
        }, 2000));
    }

    [Fact]
    public void retained_received_reuse_releases_credit_before_no_data()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var sender = context.CreatePairSocket();
        using var receiver = context.CreatePairSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc",
            "retained-received-reuse");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        using Message message = Message.From("held");
        Assert.True(sender.Send().Message(message).Submit());

        using var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                receiver.RecvRetained(received, RecvFlags.DontWait),
            2000));
        Assert.Equal(1UL, context.GetCoreHwmBudgetSnapshot()
            .OutstandingApplicationLeaseCount);

        Assert.False(receiver.RecvRetained(received, RecvFlags.DontWait));
        CoreHwmBudgetSnapshot released = context.GetCoreHwmBudgetSnapshot();
        Assert.Equal(0UL, released.OutstandingApplicationLeaseCount);
        Assert.Equal(0UL, released.ApplicationAccountedBytes);
    }

    [Fact]
    public void retained_received_releases_retired_origin_and_context_progresses()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        var retained = Received.Create();
        using (var sender = context.CreatePairSocket())
        using (var receiver = context.CreatePairSocket())
        {
            var endpoint = CoreTestSupport.NewEndpoint("inproc",
                "retained-retired-origin");
            sender.Bind(endpoint);
            receiver.Connect(endpoint);
            Thread.Sleep(50);

            using Message message = Message.From("retired");
            Assert.True(sender.Send().Message(message).Submit());
            Assert.True(CoreTestSupport.WaitUntil(() =>
                    receiver.RecvRetained(retained, RecvFlags.DontWait),
                2000));
            Assert.Equal(1UL, context.GetCoreHwmBudgetSnapshot()
                .OutstandingApplicationLeaseCount);
        }

        Assert.Equal(1UL, context.GetCoreHwmBudgetSnapshot()
            .OutstandingApplicationLeaseCount);
        retained.Dispose();
        Assert.True(CoreTestSupport.WaitUntil(() => context
                .GetCoreHwmBudgetSnapshot()
                .OutstandingApplicationLeaseCount == 0,
            2000));

        using var progressSender = context.CreatePairSocket();
        using var progressReceiver = context.CreatePairSocket();
        var progressEndpoint = CoreTestSupport.NewEndpoint("inproc",
            "retained-context-progress");
        progressSender.Bind(progressEndpoint);
        progressReceiver.Connect(progressEndpoint);
        Thread.Sleep(50);
        using Message progress = Message.From("progress");
        Assert.True(progressSender.Send().Message(progress).Submit());
        using var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                progressReceiver.Recv(received, RecvFlags.DontWait),
            2000));
        Assert.Equal("progress", received.SinglePartOrThrow().GetString());
    }

    [Fact]
    public void ordinary_topic_message_returns_credit_at_dequeue()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var publisher = context.CreatePubSocket();
        using var subscriber = context.CreateSubSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc",
            "ordinary-topic-credit");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription("credit");
        Thread.Sleep(100);

        CoreTestSupport.PublishWithRetry(publisher, "credit", "payload"u8,
            2000);
        using var topicMessage = new TopicMessage();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                subscriber.Subscribe(topicMessage, RecvFlags.DontWait),
            2000));

        CoreHwmBudgetSnapshot snapshot = context.GetCoreHwmBudgetSnapshot();
        Assert.Equal(0UL, snapshot.OutstandingApplicationLeaseCount);
        Assert.Equal(0UL, snapshot.ApplicationAccountedBytes);
        Assert.Equal(snapshot.CoreQueueAccountedBytes,
            snapshot.CurrentAccountedBytes);
    }

    [Fact]
    public void retained_topic_message_releases_credit_for_reuse()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var publisher = context.CreatePubSocket();
        using var subscriber = context.CreateSubSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc",
            "retained-topic");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription("credit");
        Thread.Sleep(100);

        CoreTestSupport.PublishWithRetry(publisher, "credit", "payload"u8,
            2000);
        using var topicMessage = new TopicMessage();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                subscriber.SubscribeRetained(topicMessage,
                    RecvFlags.DontWait),
            2000));
        Assert.Equal("credit", topicMessage.Topic);
        Assert.Equal("payload", topicMessage.SinglePartOrThrow().GetString());

        CoreHwmBudgetSnapshot held = context.GetCoreHwmBudgetSnapshot();
        Assert.Equal(1UL, held.OutstandingApplicationLeaseCount);
        Assert.True(held.ApplicationAccountedBytes > 0);

        topicMessage.ReleaseForReuse();
        Assert.True(CoreTestSupport.WaitUntil(() =>
        {
            CoreHwmBudgetSnapshot released =
                context.GetCoreHwmBudgetSnapshot();
            return released.OutstandingApplicationLeaseCount == 0
                   && released.ApplicationAccountedBytes == 0;
        }, 2000));
    }

    [Fact]
    public async Task retained_routed_receive_preserves_typed_metadata()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        RoutingId dealerRoutingId =
            CoreTestSupport.RoutingIdUtf8("retained-dealer");
        dealer.SetRoutingId(dealerRoutingId);
        var endpoint = CoreTestSupport.NewEndpoint("inproc",
            "retained-routed-metadata");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(50);

        using Message request = Message.From("ping");
        await dealer.Send().Message(request).Async();

        using var routerReceived = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                router.RecvRetained(routerReceived, RecvFlags.DontWait),
            2000));
        Assert.Equal(dealerRoutingId, routerReceived.RoutingId);
        Assert.Equal(ReceivedMessageType.Raw, routerReceived.MessageType);
        Assert.Null(routerReceived.RequestSeq);
        Assert.Equal("ping", Encoding.UTF8.GetString(
            routerReceived.FirstPart().AsReadOnlySpan()));
        Assert.Equal(1UL, context.GetCoreHwmBudgetSnapshot()
            .OutstandingApplicationLeaseCount);

        RoutingId replyTarget = routerReceived.RoutingId!.Value;
        routerReceived.Dispose();
        using Message reply = Message.From("pong");
        await router.Send(replyTarget).Message(reply).Async();

        using var dealerReceived = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(() =>
                dealer.RecvRetained(dealerReceived, RecvFlags.DontWait),
            2000));
        Assert.Equal(ReceivedMessageType.Raw, dealerReceived.MessageType);
        Assert.Null(dealerReceived.RequestSeq);
        Assert.Equal("pong", dealerReceived.FirstPart().GetString());
        Assert.Equal(1UL, context.GetCoreHwmBudgetSnapshot()
            .OutstandingApplicationLeaseCount);

        dealerReceived.Dispose();
        Assert.True(CoreTestSupport.WaitUntil(() => context
                .GetCoreHwmBudgetSnapshot()
                .OutstandingApplicationLeaseCount == 0,
            2000));
    }
}
