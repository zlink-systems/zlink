using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_tokenless_backpressure
{
    [Fact]
    public async Task full_completion_reservations_preserve_request_backpressure()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        poller.Add(dealer, PollEventFlags.PollCompletion, 1);
        dealer.Connect(CoreTestSupport.NewEndpoint("inproc",
            "tokenless-reservation-limit"));

        const int reservationLimit = 65_536;
        var pending = new Task<IReadOnlyList<Message>>[reservationLimit];
        for (var i = 0; i < pending.Length; i++)
        {
            using Message part = Message.From("reserved");
            pending[i] = dealer.Request().Message(part).Async();
            Assert.False(pending[i].IsCompleted);
        }

        try
        {
            using Message overflow = Message.From("overflow");
            ZlinkSubmitException asyncError = await Assert.ThrowsAsync<
                ZlinkSubmitException>(() => dealer.Request().Message(overflow)
                    .Async());
            AssertBackpressure(asyncError);
            Assert.Equal("overflow", overflow.GetString());

            ZlinkSubmitException blockingError = Assert.Throws<ZlinkSubmitException>(
                () => dealer.Request().Message(overflow).Submit());
            AssertBackpressure(blockingError);
            Assert.Equal("overflow", overflow.GetString());
        }
        finally
        {
            dealer.Close();
            foreach (Task<IReadOnlyList<Message>> task in pending)
            {
                ZlinkSubmitException error = await Assert.ThrowsAsync<
                    ZlinkSubmitException>(() => task);
                Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated,
                    error.Result);
            }
        }
    }

    private static void AssertBackpressure(ZlinkSubmitException error)
    {
        Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured, error.Result);
        Assert.True(error.NativeErrno is 11 or 35 or 10035,
            $"Expected EAGAIN, got {error.NativeErrno}.");
    }
}
