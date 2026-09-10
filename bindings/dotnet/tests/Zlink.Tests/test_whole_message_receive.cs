using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_whole_message_receive
{
    [Theory]
    [InlineData(RecvFlags.None)]
    [InlineData(RecvFlags.DontWait)]
    public async Task router_capacity_retry_preserves_request_order_route_and_token(RecvFlags flags)
    {
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        router.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "whole-request");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using var received = Received.Create();

        // Exceed the initial native pool capacity, then reuse the same result
        // for a single part. The second record must not be consumed by growth.
        foreach (int count in new[] { 33, 1 })
        {
            var sent = Enumerable.Range(0, count)
                .Select(i => Message.From($"request-{count}-{i}")).ToArray();
            try
            {
                var operation = dealer.Request().Message(sent[0])
                    .Timeout(TimeSpan.FromSeconds(2));
                for (int i = 1; i < count; i++)
                    operation.Message(sent[i]);
                var pending = operation.Async();
                Assert.True(CoreTestSupport.WaitUntil(() => router.Recv(received, flags), 2000));
                Assert.Equal(count, received.Parts.Count);
                Assert.Equal(dealer.GetRoutingId(), received.RoutingId);
                Assert.NotNull(received.ReplyToken);
                for (int i = 0; i < count; i++)
                    Assert.Equal($"request-{count}-{i}", received.Parts[i].GetString());
                using var response = Message.From($"reply-{count}");
                received.Reply().Message(response).Submit();
                var reply = await pending.WaitAsync(TimeSpan.FromSeconds(2));
                try
                {
                    Assert.Equal($"reply-{count}", Assert.Single(reply).GetString());
                }
                finally
                {
                    Zlink.MultipartClose(reply);
                }
            }
            finally
            {
                Zlink.MultipartClose(sent);
            }
        }
        Assert.False(router.Recv(received, RecvFlags.DontWait));
    }

    [Theory]
    [InlineData(RecvFlags.None)]
    [InlineData(RecvFlags.DontWait)]
    public void subscription_capacity_retry_preserves_topic_parts_and_next_record(RecvFlags flags)
    {
        using var context = Zlink.CreateContext();
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        subscriber.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "whole-subscribe");
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(string.Empty);
        var subscription = new SubscriptionEvent();
        Assert.True(CoreTestSupport.WaitUntil(
            () => publisher.ReceiveSubscriptionEvent(subscription, RecvFlags.DontWait), 2000));
        Assert.True(subscription.Subscribed);

        foreach (int count in new[] { 33, 1 })
        {
            var sent = Enumerable.Range(0, count)
                .Select(i => Message.From($"part-{count}-{i}")).ToArray();
            try
            {
                var operation = publisher.Publish($"topic-{count}").Message(sent[0]);
                for (int i = 1; i < count; i++)
                    operation.Message(sent[i]);
                operation.Submit();
            }
            finally
            {
                Zlink.MultipartClose(sent);
            }
        }

        using var received = new TopicMessage();
        foreach (int count in new[] { 33, 1 })
        {
            Assert.True(CoreTestSupport.WaitUntil(
                () => subscriber.Subscribe(received, flags), 2000));
            Assert.Equal($"topic-{count}", received.Topic);
            Assert.Equal(count, received.Parts.Count);
            for (int i = 0; i < count; i++)
                Assert.Equal($"part-{count}-{i}", received.Parts[i].GetString());
        }
        Assert.False(subscriber.Subscribe(received, RecvFlags.DontWait));
        Assert.Equal("part-1-0", received.SinglePartOrThrow().GetString());
    }
}
