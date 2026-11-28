// SPDX-License-Identifier: MPL-2.0

using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_consumed_message_submit
{
    [Fact]
    public async Task consumed_message_is_rejected_by_each_submit_operation()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var publisher = context.CreatePubSocket();
        using var completions = new CompletionPollerDriver(dealer);
        using var received = Received.Create();
        using var consumed = Message.From("probe");
        using var first = Message.From("preserved");

        var endpoint = CoreTestSupport.NewEndpoint("inproc", "consumed-message");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        dealer.Send().Message(consumed).Submit();
        Assert.True(router.Recv(received));
        Assert.Equal("probe", received.FirstPart().GetString());

        using var ping = Message.From("ping");
        var pending = dealer.Request().Message(ping)
            .Timeout(TimeSpan.FromSeconds(5)).Async();
        Assert.True(router.Recv(received));
        AssertInvalidHandle(() => dealer.Send().Message(consumed).Submit());
        AssertInvalidHandle(() => dealer.Send().Message(first)
            .Message(consumed).Async());
        AssertInvalidHandle(() => dealer.Request().Message(consumed).Submit());
        AssertInvalidHandle(() => dealer.Request().Message(consumed).Async());
        AssertInvalidHandle(() => received.Reply().Message(consumed).Submit());
        AssertInvalidHandle(() => publisher.Publish("topic").Message(consumed)
            .Submit());
        AssertInvalidHandle(() => publisher.Publish("topic").Message(first)
            .Message(consumed).Submit());

        Assert.Equal(0, consumed.Size);
        Assert.Empty(consumed.ToArray());
        Assert.Equal("preserved", first.GetString());

        using var pong = Message.From("pong");
        received.Reply().Message(pong).Submit();
        var reply = await pending.Reply.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            Assert.Equal("pong", Assert.Single(reply).GetString());
        }
        finally
        {
            Zlink.MultipartClose(reply);
        }
        Assert.False(router.Recv(received, RecvFlags.DontWait));
    }

    private static void AssertInvalidHandle(Action submit)
    {
        var error = Assert.Throws<ZlinkSubmitException>(submit);
        Assert.Equal(ZlinkSubmitException.ErrorCode.InvalidHandle, error.Result);
    }
}
