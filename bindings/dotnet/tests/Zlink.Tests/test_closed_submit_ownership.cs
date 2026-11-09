using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_closed_submit_ownership
{
    public static IEnumerable<object[]> Cases()
    {
        foreach (string kind in new[] { "send", "send-async", "request",
                     "request-async", "reply", "received-reply", "publish", "try-publish" })
            foreach (int count in new[] { 1, 2, 9 })
                yield return new object[] { kind, count };
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public void refused_submit_preserves_every_message(string kind, int count)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var publisher = context.CreatePubSocket();
        using var received = Received.Create();
        using var poller = Zlink.CreatePoller();
        Task<IReadOnlyList<Message>>? pending = null;
        if (kind is "reply" or "received-reply")
        {
            string endpoint = CoreTestSupport.NewEndpoint("inproc", "closed-reply");
            router.Bind(endpoint);
            dealer.Connect(endpoint);
            using (Message handshake = Message.From("ready"))
                dealer.Send().Message(handshake).Submit();
            Assert.True(router.Recv(received));
            poller.Add(dealer, PollEventFlags.PollCompletion, 1);
            using Message request = Message.From("request");
            pending = dealer.Request().Message(request)
                .Timeout(TimeSpan.FromSeconds(30)).Async().Reply;
            Assert.True(router.Recv(received));
        }

        ISocket subject = kind is "reply" or "received-reply" ? router
            : kind is "publish" or "try-publish" ? publisher : dealer;
        subject.Close();
        Message[] parts = Enumerable.Range(0, count)
            .Select(index => Message.From($"part-{index}")).ToArray();
        try
        {
            ZlinkSubmitException error = Assert.Throws<ZlinkSubmitException>(() =>
            {
                switch (kind)
                {
                    case "send":
                        dealer.Send().Messages(parts).Submit();
                        break;
                    case "send-async":
                        dealer.Send().Messages(parts).Async();
                        break;
                    case "request":
                        Zlink.MultipartClose(dealer.Request().Messages(parts)
                            .Timeout(TimeSpan.FromSeconds(3)).Submit());
                        break;
                    case "request-async":
                        dealer.Request().Messages(parts)
                            .Timeout(TimeSpan.FromSeconds(3)).Async();
                        break;
                    case "reply":
                        router.Reply(received.RoutingId!.Value, received.ReplyToken!)
                            .Messages(parts).Submit();
                        break;
                    case "received-reply":
                        received.Reply().Messages(parts).Submit();
                        break;
                    case "publish":
                        publisher.Publish("topic").Messages(parts).Submit();
                        break;
                    case "try-publish":
                        TryPublishSubmitOperation operation = publisher.TryPublish("topic")
                            .Message(parts[0]);
                        for (var index = 1; index < parts.Length; index++)
                            operation = operation.Message(parts[index]);
                        operation.Submit();
                        break;
                }
            });
            Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
            Assert.Equal(CoreTestSupport.Errno("ESHUTDOWN"), error.NativeErrno);
            for (var index = 0; index < parts.Length; index++)
                Assert.Equal($"part-{index}", parts[index].GetString());
        }
        finally
        {
            Zlink.MultipartClose(parts);
            dealer.Close();
            if (pending is not null)
                Assert.Throws<ZlinkRequestException>(() => pending.GetAwaiter().GetResult());
        }
    }
}
